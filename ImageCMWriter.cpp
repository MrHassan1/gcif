#include "ImageCMWriter.hpp"
#include "BitMath.hpp"
#include "Filters.hpp"
#include "EntropyEstimator.hpp"
#include "Log.hpp"
#include "ImageLZWriter.hpp"
#include "GCIFWriter.hpp"
using namespace cat;

#include <vector>
using namespace std;

#include "lz4.h"
#include "lz4hc.h"
#include "Log.hpp"
#include "HuffmanEncoder.hpp"
#include "lodepng.h"

#include <iostream>
using namespace std;


static CAT_INLINE int score(u8 p) {
	if (p < 128) {
		return p;
	} else {
		return 256 - p;
	}
}

static CAT_INLINE int scoreYUV(u8 *yuv) {
	return score(yuv[0]) + score(yuv[1]) + score(yuv[2]);
}

static CAT_INLINE int wrapNeg(u8 p) {
	if (p == 0) {
		return 0;
	} else if (p < 128) {
		return ((p - 1) << 1) | 1;
	} else {
		return (256 - p) << 1;
	}
}


//// ImageCMWriter

void ImageCMWriter::clear() {
	if (_matrix) {
		delete []_matrix;
		_matrix = 0;
	}
	if (_chaos) {
		delete []_chaos;
		_chaos = 0;
	}
}

int ImageCMWriter::init(int width, int height) {
	clear();

	if (width < FILTER_ZONE_SIZE || height < FILTER_ZONE_SIZE) {
		return WE_BAD_DIMS;
	}

	if ((width & FILTER_ZONE_SIZE_MASK) || (height & FILTER_ZONE_SIZE_MASK)) {
		return WE_BAD_DIMS;
	}

	_width = width;
	_height = height;

	_w = width >> FILTER_ZONE_SIZE_SHIFT;
	_h = height >> FILTER_ZONE_SIZE_SHIFT;
	_matrix = new u16[_w * _h];

	// And last row of chaos data
	_chaos_size = (width + RECENT_SYMS) * PLANES;
	_chaos = new u8[_chaos_size];

	return WE_OK;
}

void ImageCMWriter::decideFilters() {
	EntropyEstimator<u8> ee[3];
	for (int ii = 0; ii < 3; ++ii) {
		ee[ii].clear(256);
	}

	FilterScorer scores;
	scores.init(SF_COUNT * CF_COUNT);

	int compressLevel = COMPRESS_LEVEL;
	const int width = _width;

	for (int y = 0; y < _height; y += FILTER_ZONE_SIZE) {
		for (int x = 0; x < width; x += FILTER_ZONE_SIZE) {

			// Determine best filter combination to use
			int bestSF = 0, bestCF = 0;

			// If filter zone has RGB data,
#ifdef LOWRES_MASK
			if (!_mask->hasRGB(x, y))
#endif
			{
				// Lower compression level that is a lot faster:
				if (compressLevel == 0) {
					scores.reset();

					// For each pixel in the 8x8 zone,
					for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
						for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
							int px = x + xx, py = y + yy;
#ifndef LOWRES_MASK
							if (_mask->hasRGB(px, py)) {
								continue;
							}
#endif
							if (_lz->visited(px, py)) {
								continue;
							}

							const u8 *p = _rgba + (px + py * width) * 4;

							for (int ii = 0; ii < SF_COUNT; ++ii) {
								const u8 *pred = SPATIAL_FILTERS[ii](p, px, py, width);

								u8 temp[3];
								for (int jj = 0; jj < 3; ++jj) {
									temp[jj] = p[jj] - pred[jj];
								}

								for (int jj = 0; jj < CF_COUNT; ++jj) {
									u8 yuv[3];
									RGB2YUV_FILTERS[jj](temp, yuv);

									int error = scoreYUV(yuv);

									scores.add(ii + jj*SF_COUNT, error);
								}
							}
						}
					}

					FilterScorer::Score *best = scores.getLowest();

					// Write it out
					bestSF = best->index % SF_COUNT;
					bestCF = best->index / SF_COUNT;

				} else { // Higher compression level that uses entropy estimate:

					scores.reset();

					// For each pixel in the 8x8 zone,
					for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
						for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
							int px = x + xx, py = y + yy;
#ifndef LOWRES_MASK
							if (_mask->hasRGB(px, py)) {
								continue;
							}
#endif
							if (_lz->visited(px, py)) {
								continue;
							}

							const u8 *p = _rgba + (px + py * width) * 4;

							for (int ii = 0; ii < SF_COUNT; ++ii) {
								const u8 *pred = SPATIAL_FILTERS[ii](p, px, py, width);
								u8 temp[3];
								for (int jj = 0; jj < 3; ++jj) {
									temp[jj] = p[jj] - pred[jj];
								}

								for (int jj = 0; jj < CF_COUNT; ++jj) {
									u8 yuv[3];
									RGB2YUV_FILTERS[jj](temp, yuv);

									int error = scoreYUV(yuv);

									scores.add(ii + SF_COUNT*jj, error);
								}
							}
						}
					}


					FilterScorer::Score *lowest = scores.getLowest();

					if (lowest->score <= 4) {
						bestSF = lowest->index % SF_COUNT;
						bestCF = lowest->index / SF_COUNT;
					} else {
						const int TOP_COUNT = FILTER_SELECT_FUZZ;

						FilterScorer::Score *top = scores.getTop(TOP_COUNT);

						double bestScore = 0;

						for (int ii = 0; ii < TOP_COUNT; ++ii) {
							// Write it out
							u8 sf = top[ii].index % SF_COUNT;
							u8 cf = top[ii].index / SF_COUNT;

							for (int jj = 0; jj < 3; ++jj) {
								ee[jj].setup();
							}

							for (int yy = 0; yy < FILTER_ZONE_SIZE; ++yy) {
								for (int xx = 0; xx < FILTER_ZONE_SIZE; ++xx) {
									int px = x + xx, py = y + yy;
#ifndef LOWRES_MASK
									if (_mask->hasRGB(px, py)) {
										continue;
									}
#endif
									if (_lz->visited(px, py)) {
										continue;
									}

									const u8 *p = _rgba + (px + py * width) * 4;
									const u8 *pred = SPATIAL_FILTERS[sf](p, px, py, width);
									u8 temp[3];
									for (int jj = 0; jj < 3; ++jj) {
										temp[jj] = p[jj] - pred[jj];
									}

									u8 yuv[3];
									RGB2YUV_FILTERS[cf](temp, yuv);

									ee[0].push(yuv[0]);
									ee[1].push(yuv[1]);
									ee[2].push(yuv[2]);
								}
							}

							double score = ee[0].entropy() + ee[1].entropy() + ee[2].entropy();
							if (ii == 0) {
								bestScore = score;
								bestSF = sf;
								bestCF = cf;
								for (int jj = 0; jj < 3; ++jj) {
									ee[jj].save();
								}
							} else {
								if (score < bestScore) {
									bestSF = sf;
									bestCF = cf;
									for (int jj = 0; jj < 3; ++jj) {
										ee[jj].save();
									}
									bestScore = score;
								}
							}
						}

						for (int jj = 0; jj < 3; ++jj) {
							ee[jj].commit();
						}
					}
				}
			}

			u16 filter = ((u16)bestSF << 8) | bestCF;
			setFilter(x, y, filter);
		}
	}
}

void ImageCMWriter::chaosStats() {
#ifdef GENERATE_CHAOS_TABLE
	GenerateChaosTable();
#endif

	const int width = _width;

	// For each scanline,
	const u8 *p = _rgba;
	u8 *lastStart = _chaos + RECENT_SYMS * PLANES;
	CAT_CLR(lastStart, _chaos_size);

	for (int y = 0; y < _height; ++y) {
		u8 *last = lastStart;

		// For each pixel,
		for (int x = 0; x < width; ++x) {
			// If not masked out,
			if (!_lz->visited(x, y) && !_mask->hasRGB(x, y)) {
				// Get filter for this pixel
				u16 filter = getFilter(x, y);
				u8 cf = (u8)filter;
				u8 sf = (u8)(filter >> 8);

				// Apply spatial filter
				const u8 *pred = SPATIAL_FILTERS[sf](p, x, y, width);
				u8 temp[3];
				for (int jj = 0; jj < 3; ++jj) {
					temp[jj] = p[jj] - pred[jj];
				}

				// Apply color filter
				u8 yuv[PLANES];
				RGB2YUV_FILTERS[cf](temp, yuv);
				if (x > 0) {
					yuv[3] = p[-1] - p[3];
				} else {
					yuv[3] = 255 - p[3];
				}

				int match = -1;
				int nonzero = (yuv[0] != 0) + (yuv[1] != 0) + (yuv[2] != 0) + (yuv[3] != 0);

				if (nonzero >= 0) {
					for (int ii = 0; ii < RECENT_SYMS; ++ii) {
						const int offset = ii - RECENT_AHEAD;

						if (yuv[0] == last[0 - offset * 4] &&
								yuv[1] == last[1 - offset * 4] &&
								yuv[2] == last[2 - offset * 4] &&
								yuv[3] == last[3 - offset * 4]) {
							match = ii;
							break;
						}
					}
				}

				// For each color,
				for (int c = 0; c < PLANES; ++c) {
					// Measure context chaos
					u8 chaos = CHAOS_TABLE[chaosScore(last[c - 4]) + chaosScore(last[c])];

					if (match == -1) {
						_encoder[c][chaos].push(yuv[c]);
					} else {
						if (c == 0) {
							_encoder[0][chaos].push(256 + match);
						}
					}

					// Store chaos score for next time
					last[c] = yuv[c];
				}
			} else {
				for (int c = 0; c < PLANES; ++c) {
					last[c] = 0;
				}
			}

			// Next pixel
			last += PLANES;
			p += 4;
		}
	}

	// Finalize
	for (int ii = 0; ii < PLANES; ++ii) {
		for (int jj = 0; jj < CHAOS_LEVELS; ++jj) {
			_encoder[ii][jj].finalize();
		}
	}
}



#if 0

void colorSpace(u8 *rgba, int width, int height, ImageMaskWriter &mask) {
	for (int cf = 0; cf < CF_COUNT; ++cf) {
		EntropyEstimator<u32> ee[3];
		for (int ii = 0; ii < 3; ++ii) {
			ee[ii].clear(256);
			ee[ii].setup();
		}

		u8 *p = rgba;
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				u8 yuv[3];
				convertRGBtoYUV(cf, p, yuv);

				ee[0].push(yuv[0]);
				ee[1].push(yuv[1]);
				ee[2].push(yuv[2]);

				p += 4;
			}
		}
/*
		if (cf == CF_YCgCo_R) {
			ee[0].drawHistogram(rgba, width);
			ee[1].drawHistogram(rgba + 800, width);
			ee[2].drawHistogram(rgba + 1600, width);
			return;
		}
*/
		double e[3], score = 0;
		for (int ii = 0; ii < 3; ++ii) {
			e[ii] = ee[ii].entropy();
			score += e[ii];
		}

		cout << "YUV888 Entropy for " << GetColorFilterString(cf) << " = { " << e[0] << ", " << e[1] << ", " << e[2] << " } : SCORE=" << score << endl;
	}
}

#endif

int ImageCMWriter::initFromRGBA(const u8 *rgba, int width, int height, ImageMaskWriter &mask, ImageLZWriter &lz) {
	int err;

	if ((err = init(width, height))) {
		return err;
	}

	_rgba = rgba;
	_mask = &mask;
	_lz = &lz;

#ifdef TEST_COLOR_FILTERS
	testColorFilters();
	//colorSpace(rgba, width, height, mask);
	return -1;
#endif

	decideFilters();

	chaosStats();

	return WE_OK;
}

bool ImageCMWriter::writeFilters(ImageWriter &writer) {
	FreqHistogram<SF_COUNT> sf_hist;
	FreqHistogram<CF_COUNT> cf_hist;
	u32 unused_count = 0;

	// For each zone,
	for (int y = 0, height = _height; y < height; y += FILTER_ZONE_SIZE) {
		for (int x = 0, width = _width; x < width; x += FILTER_ZONE_SIZE) {
			// Encode SF and CF separately and combine consecutive filters
			// together for the smallest representation
#ifdef LOWRES_MASK
			if (!_mask->hasRGB(x, y))
#else
			bool on = false;

			for (int ii = 0; ii < FILTER_ZONE_SIZE; ++ii) {
				for (int jj = 0; jj < FILTER_ZONE_SIZE; ++jj) {
					if (!_lz->visited(x + ii, y + jj) && !_mask->hasRGB(x + ii, y + jj)) {
						on = true;
						ii = FILTER_ZONE_SIZE;
						break;
					}
				}
			}

			if (on)
#endif
			{
				u16 filter = getFilter(x, y);
				u8 cf = (u8)filter;
				u8 sf = (u8)(filter >> 8);

				sf_hist.add(sf);
				cf_hist.add(cf);
			} else {
				setFilter(x, y, UNUSED_FILTER);

				unused_count++;
			}
		}
	}

	// Use most common symbol as unused symbol
	_sf_unused_sym = sf_hist.firstHighestPeak();
	_cf_unused_sym = cf_hist.firstHighestPeak();

	// Add unused count onto it
	sf_hist.addMore(_sf_unused_sym, unused_count);
	cf_hist.addMore(_cf_unused_sym, unused_count);

	// Geneerate huffman codes from final histogram
	if (!_sf_encoder.init(sf_hist)) {
		return false;
	}
	if (!_cf_encoder.init(cf_hist)) {
		return false;
	}

	// Write out filter huffman tables
	int sf_table_bits = _sf_encoder.writeTable(writer);
	int cf_table_bits = _cf_encoder.writeTable(writer);
#ifdef CAT_COLLECT_STATS
	Stats.filter_table_bits[0] = sf_table_bits;
	Stats.filter_table_bits[1] = cf_table_bits;
#endif // CAT_COLLECT_STATS

	return true;
}

bool ImageCMWriter::writeChaos(ImageWriter &writer) {
#ifdef CAT_COLLECT_STATS
	int overhead_bits = 0;
	int bitcount[PLANES] = {0};
	int chaos_count = 0;
	int filter_table_bits[2] = {0};
#endif

	for (int ii = 0; ii < PLANES; ++ii) {
		for (int jj = 0; jj < CHAOS_LEVELS; ++jj) {
			int bits = _encoder[ii][jj].writeOverhead(writer);
#ifdef CAT_COLLECT_STATS
			overhead_bits += bits;
#endif
		}
	}

	const int width = _width;

	// For each scanline,
	const u8 *p = _rgba;
	u8 *lastStart = _chaos + RECENT_SYMS * PLANES;
	CAT_CLR(lastStart, _chaos_size);

	for (int y = 0; y < _height; ++y) {
		u8 *last = lastStart;

		// For each pixel,
		for (int x = 0; x < width; ++x) {
			// If it is time to write out a filter,
			if ((x & FILTER_ZONE_SIZE_MASK) == 0 &&
				(y & FILTER_ZONE_SIZE_MASK) == 0) {
				u16 filter = getFilter(x, y);
				u8 sf, cf;

				// If filter is unused,
				if (filter == UNUSED_FILTER) {
					sf = _sf_unused_sym;
					cf = _cf_unused_sym;
				} else {
					sf = filter >> 8;
					cf = (u8)filter;
				}

				int sf_bits = _sf_encoder.writeSymbol(sf, writer);
				int cf_bits = _cf_encoder.writeSymbol(cf, writer);
#ifdef CAT_COLLECT_STATS
				filter_table_bits[0] += sf_bits;
				filter_table_bits[1] += cf_bits;
#endif
			}

			// If not masked out,
			if (!_lz->visited(x, y) && !_mask->hasRGB(x, y)) {
				// Get filter for this pixel
				u16 filter = getFilter(x, y);
				u8 cf = (u8)filter;
				u8 sf = (u8)(filter >> 8);

				// Apply spatial filter
				const u8 *pred = SPATIAL_FILTERS[sf](p, x, y, width);
				u8 temp[3];
				for (int jj = 0; jj < 3; ++jj) {
					temp[jj] = p[jj] - pred[jj];
				}

				// Apply color filter
				u8 yuv[PLANES];
				RGB2YUV_FILTERS[cf](temp, yuv);
				if (x > 0) {
					yuv[3] = p[-1] - p[3];
				} else {
					yuv[3] = 255 - p[3];
				}

				int match = -1;
				int nonzero = (yuv[0] != 0) + (yuv[1] != 0) + (yuv[2] != 0) + (yuv[3] != 0);

				if (nonzero >= 0) {
					for (int ii = 0; ii < RECENT_SYMS; ++ii) {
						const int offset = ii - RECENT_AHEAD;

						if (yuv[0] == last[0 - offset * 4] &&
								yuv[1] == last[1 - offset * 4] &&
								yuv[2] == last[2 - offset * 4] &&
								yuv[3] == last[3 - offset * 4]) {
							match = ii;
							break;
						}
					}
				}

				// For each color,
				for (int c = 0; c < PLANES; ++c) {
					// Measure context chaos
					u8 chaos = CHAOS_TABLE[chaosScore(last[c - 4]) + chaosScore(last[c])];

					if (match == -1) {
						// Record YUV
						int bits = _encoder[c][chaos].encode(yuv[c], writer);
#ifdef CAT_COLLECT_STATS
						bitcount[c] += bits;
#endif
					} else {
						if (c == 0) {
							// Record YUV
							int bits = _encoder[c][chaos].encode(256 + match, writer);
#ifdef CAT_COLLECT_STATS
							bitcount[c] += bits;
#endif
						}
					}

					// Store chaos score for next time
					last[c] = yuv[c];
				}
#ifdef CAT_COLLECT_STATS
				chaos_count++;
#endif
			} else {
				for (int c = 0; c < PLANES; ++c) {
					last[c] = 0;
				}
			}

			// Next pixel
			last += PLANES;
			p += 4;
		}
	}

#ifdef CAT_COLLECT_STATS
	for (int ii = 0; ii < PLANES; ++ii) {
		Stats.rgb_bits[ii] = bitcount[ii];
	}
	Stats.chaos_overhead_bits = overhead_bits;
	Stats.chaos_count = chaos_count;
	Stats.filter_compressed_bits[0] = filter_table_bits[0];
	Stats.filter_compressed_bits[1] = filter_table_bits[1];
#endif

	return true;
}

void ImageCMWriter::write(ImageWriter &writer) {
	writeFilters(writer);

	writeChaos(writer);

#ifdef CAT_COLLECT_STATS
	int total = 0;
	for (int ii = 0; ii < 2; ++ii) {
		total += Stats.filter_table_bits[ii];
		total += Stats.filter_compressed_bits[ii];
	}
	for (int ii = 0; ii < PLANES; ++ii) {
		total += Stats.rgb_bits[ii];
	}
	total += Stats.chaos_overhead_bits;
	Stats.chaos_bits = total;
	total += _lz->Stats.huff_bits;
	total += _mask->Stats.compressedDataBits;
	Stats.total_bits = total;

	Stats.overall_compression_ratio = _width * _height * 4 * 8 / (double)Stats.total_bits;

	Stats.chaos_compression_ratio = Stats.chaos_count * PLANES * 8 / (double)Stats.chaos_bits;
#endif
}

#ifdef CAT_COLLECT_STATS

bool ImageCMWriter::dumpStats() {
	CAT_INANE("stats") << "(CM Compress) Spatial Filter Table Size : " <<  Stats.filter_table_bits[0] << " bits (" << Stats.filter_table_bits[0]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Spatial Filter Compressed Size : " <<  Stats.filter_compressed_bits[0] << " bits (" << Stats.filter_compressed_bits[0]/8 << " bytes)";

	CAT_INANE("stats") << "(CM Compress) Color Filter Table Size : " <<  Stats.filter_table_bits[1] << " bits (" << Stats.filter_table_bits[1]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Color Filter Compressed Size : " <<  Stats.filter_compressed_bits[1] << " bits (" << Stats.filter_compressed_bits[1]/8 << " bytes)";

	CAT_INANE("stats") << "(CM Compress) Y-Channel Compressed Size : " <<  Stats.rgb_bits[0] << " bits (" << Stats.rgb_bits[0]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) U-Channel Compressed Size : " <<  Stats.rgb_bits[1] << " bits (" << Stats.rgb_bits[1]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) V-Channel Compressed Size : " <<  Stats.rgb_bits[2] << " bits (" << Stats.rgb_bits[2]/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) A-Channel Compressed Size : " <<  Stats.rgb_bits[3] << " bits (" << Stats.rgb_bits[3]/8 << " bytes)";

	CAT_INANE("stats") << "(CM Compress) YUVA Overhead Size : " << Stats.chaos_overhead_bits << " bits (" << Stats.chaos_overhead_bits/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Chaos pixel count : " << Stats.chaos_count << " pixels";
	CAT_INANE("stats") << "(CM Compress) Chaos compression ratio : " << Stats.chaos_compression_ratio << ":1";
	CAT_INANE("stats") << "(CM Compress) Overall size : " << Stats.total_bits << " bits (" << Stats.total_bits/8 << " bytes)";
	CAT_INANE("stats") << "(CM Compress) Overall compression ratio : " << Stats.overall_compression_ratio << ":1";

	return true;
}

#endif

