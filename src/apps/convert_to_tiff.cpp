/***************************************************************************
 *
 * Author: "Takanori Nakane"
 * MRC Laboratory of Molecular Biology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * This complete copyright notice must be included in any revised version of the
 * source code. Additional authorship citations may be added, but existing
 * author citations must be preserved.
 ***************************************************************************/

#include <cstdio>
#include <cmath>
#include <src/args.h>
#include <src/image.h>
#include <src/metadata_table.h>

#ifndef HAVE_TIFF
int main(int argc, char *argv[])
{
	REPORT_ERROR("To use this program, please recompile with libtiff.");
}
#else

#include <tiffio.h>

class convert_to_tiff
{
public:
	FileName fn_in, fn_out, fn_gain, fn_compression;
	bool do_estimate, input_type, lossy, dont_die_on_error, line_by_line, only_do_unfinished;
	int deflate_level, thresh_reliable, nr_threads;
	IOParser parser;

	MetaDataTable MD;
	Image<short> defects;
	Image<float> gain;
	int nn, ny, nx, mrc_mode;

	void usage()
	{
		parser.writeUsage(std::cerr);
	}

	void read(int argc, char **argv)
	{
		parser.setCommandLine(argc, argv);

		int general_section = parser.addSection("General Options");
		fn_in = parser.getOption("--i", "Input movie to be compressed (a MRC file or a STAR file)");
		fn_out = parser.getOption("--o", "Rootname for output TIFF files", "");
		fn_gain = parser.getOption("--gain", "Estimated gain map and its reliablity map (read)", "");
		nr_threads = textToInteger(parser.getOption("--j", "Number of threads (More than 2 is not effective)", "1"));
		only_do_unfinished = parser.checkOption("--only_do_unfinished", "Only process non-converted movies.");
		thresh_reliable = textToInteger(parser.getOption("--thresh", "Number of success needed to consider a pixel reliable", "20"));
		do_estimate = parser.checkOption("--estimate_gain", "Estimate gain");

		int tiff_section = parser.addSection("TIFF options");
		fn_compression = parser.getOption("--compression", "compression type (none, auto, deflate (= zip), lzw)", "auto");
		deflate_level = textToInteger(parser.getOption("--deflate_level", "deflate level. 1 (fast) to 9 (slowest but best compression)", "6"));
		//lossy = parser.checkOption("--lossy", "Allow slightly lossy but better compression on defect pixels");
		dont_die_on_error = parser.checkOption("--ignore_error", "Don't die on un-expected defect pixels (can be dangerous)");
		line_by_line = parser.checkOption("--line_by_line", "Use one strip per row");

		if (parser.checkForErrors())
			REPORT_ERROR("Errors encountered on the command line (see above), exiting...");
	}

	template <typename T>
	void write_tiff_one_page(TIFF *tif, MultidimArray<T> buf, const int filter=COMPRESSION_LZW, const int level=6)
	{
		TIFFSetField(tif, TIFFTAG_SOFTWARE, "relion_convert_to_tiff");
		TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, XSIZE(buf));
		TIFFSetField(tif, TIFFTAG_IMAGELENGTH, YSIZE(buf));
		TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, line_by_line ? 1 : YSIZE(buf));
		TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
		TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
		TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);		

		if (std::is_same<T, float>::value)
		{
			TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 32);
			TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
		}
		else if (std::is_same<T, short>::value)
		{
			TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
			TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
		}
		else if (std::is_same<T, char>::value || std::is_same<T, unsigned char>::value )
		{
			TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
			TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
		}
		else
		{
			REPORT_ERROR("write_tiff_one_page: unknown data type");
		}

		// compression is COMPRESSION_LZW or COMPRESSION_DEFLATE or COMPRESSION_NONE
		TIFFSetField(tif, TIFFTAG_COMPRESSION, filter);
		if (filter == COMPRESSION_DEFLATE)
		{
			if (level <= 0 || level > 9)
				REPORT_ERROR("Deflate level must be 1, 2, ..., 9");
			TIFFSetField(tif, TIFFTAG_ZIPQUALITY, level);
		}

		// Have to flip the Y axis
		for (int iy = 0; iy < YSIZE(buf); iy++)
			TIFFWriteScanline(tif, buf.data + (ny - 1 - iy) * XSIZE(buf), iy, 0);

		TIFFWriteDirectory(tif);
	}

	void estimate(FileName fn_movie)
	{
		Image<float> frame;

		for (int iframe = 0; iframe < nn; iframe++)
		{
			int error = 0, changed = 0, stable = 0, negative = 0;
			
			frame.read(fn_movie, true, iframe, false, true);

			#pragma omp parallel for num_threads(nr_threads) reduction(+:error, changed, negative)
			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(frame())
			{
				const float val = DIRECT_MULTIDIM_ELEM(frame(), n);
				const float gain_here = DIRECT_MULTIDIM_ELEM(gain(), n);

				if (val == 0)
				{
					continue;
				}
				else if (val < 0)
				{
//#define DEBUG
#ifdef DEBUG
					printf(" negative: frame %2d pos %4d %4d obs % 8.4f gain %.4f\n", 
					       iframe, n / nx, n % ny, (double)val, (double)gain_here);
#endif
					negative++;
					DIRECT_MULTIDIM_ELEM(defects(), n) = -1;
				}
				else if (gain_here > val)
				{
					DIRECT_MULTIDIM_ELEM(gain(), n) = val;
					changed++;
					DIRECT_MULTIDIM_ELEM(defects(), n) = 0;
				}
				else
				{
					const int ival = (int)round(val / gain_here);
					const float expected = gain_here * ival;
					if (fabs(expected - val) > 0.0001)
					{
#ifdef DEBUG
						printf(" mismatch: frame %2d pos %4d %4d obs % 8.4f expected % 8.4f gain %.4f\n",
						       iframe, n / nx, n % ny, (double)val,
						       (double)expected, (double)gain_here);
#endif
						error++;
						DIRECT_MULTIDIM_ELEM(defects(), n) = -1;
					}
					else if (DIRECT_MULTIDIM_ELEM(defects(), n) >= 0)
					{
						DIRECT_MULTIDIM_ELEM(defects(), n)++;
					}
				}

			}

			#pragma omp parallel for num_threads(nr_threads) reduction(+:stable)
			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(defects())
			{
				short val = DIRECT_MULTIDIM_ELEM(defects(), n);
				if (val >= thresh_reliable)
					stable++;
			}

			printf(" Frame %03d #Changed %10d #Mismatch %10d, #Negative %10d, #Unreliable %10d / %10d\n",
			       iframe, changed, error, negative, ny * nx - stable, ny * nx);
		}
	}

	int decide_filter(int nx)
	{
		if (fn_compression == "none")
			return COMPRESSION_NONE;
		else if (fn_compression == "lzw")
			return COMPRESSION_LZW;
		else if (fn_compression == "deflate" || fn_compression == "zip")
			return COMPRESSION_DEFLATE;
		else if (fn_compression == "auto")
		{
			if (nx == 4096)
				return COMPRESSION_DEFLATE; // likely Falcon
			else
				return COMPRESSION_LZW;
		}
		else
			REPORT_ERROR("Compression type must be one of none, auto, deflate (= zip) or lzw.");

		return -1;
	}

	template <typename T>
	void unnormalise(FileName fn_movie, FileName fn_tiff)
	{
		FileName fn_tmp = fn_tiff + ".tmp";
		TIFF *tif = TIFFOpen(fn_tmp.c_str(), "w");
		if (tif == NULL)
			REPORT_ERROR("Failed to open the output TIFF file.");

		Image<float> frame;
		MultidimArray<T> buf(ny, nx);
		char msg[256];
	
		for (int iframe = 0; iframe < nn; iframe++)
		{
			int error = 0;
			
			frame.read(fn_movie, true, iframe, false, true);

			#pragma omp parallel for num_threads(nr_threads) reduction(+:error)
			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(frame())
			{
				const float val = DIRECT_MULTIDIM_ELEM(frame(), n);
				const float gain_here = DIRECT_MULTIDIM_ELEM(gain(), n);
				bool is_bad = DIRECT_MULTIDIM_ELEM(defects(), n) < thresh_reliable;
				
				if (is_bad)
				{
					// TODO: implement other strategy
					DIRECT_MULTIDIM_ELEM(buf, n) = val;
					continue;
				}

				int ival = (int)round(val / gain_here);
				const float expected = gain_here * ival;
				if (fabs(expected - val) > 0.0001)
				{
					snprintf(msg, 255, " mismatch: frame %2d pos %4d %4d status %5d obs % 8.4f expected % 8.4f gain %.4f\n",
					       iframe, n / nx, n % ny, (double)val, DIRECT_MULTIDIM_ELEM(defects(), n),
					       (double)expected, (double)gain_here);
					std::cerr << "mismatch" << msg << std::endl;
					if (!dont_die_on_error)
						REPORT_ERROR("Unexpected pixel value in a pixel that was considered reliable");
					error++;
				}

				if (!std::is_same<T, float>::value)
				{
					const int overflow = std::is_same<T, short>::value ? 32767: 127;
					const int underflow = std::is_same<T, short>::value ? -32768: 0;

					if (ival < underflow)
					{
						ival = underflow;
						error++;
						
						printf(" underflow: frame %2d pos %4d %4d obs % 8.4f expected % 8.4f gain %.4f\n",
					               iframe, n / nx, n % ny, (double)val,
					               (double)expected, (double)gain_here);
					}
					else if (ival > overflow)
					{
						ival = overflow;
						error++;

						printf(" overflow: frame %2d pos %4d %4d obs % 8.4f expected % 8.4f gain %.4f\n",
					               iframe, n / nx, n % ny, (double)val,
					               (double)expected, (double)gain_here);
					}
				}
				
				DIRECT_MULTIDIM_ELEM(buf, n) = ival;
			}

			write_tiff_one_page(tif, buf, decide_filter(nx), deflate_level);
			printf(" Frame %3d / %3d #Error %10d\n", iframe + 1, nn, error);
		}

		TIFFClose(tif);
		std::rename(fn_tmp.c_str(), fn_tiff.c_str());
	}

	template <typename T>
	void only_compress(FileName fn_movie, FileName fn_tiff)
	{
		FileName fn_tmp = fn_tiff + ".tmp";
		TIFF *tif = TIFFOpen(fn_tiff.c_str(), "w");
		if (tif == NULL)
			REPORT_ERROR("Failed to open the output TIFF file.");

		Image<T> frame;
		for (int iframe = 0; iframe < nn; iframe++)
		{
			frame.read(fn_movie, true, iframe, false, true);
			write_tiff_one_page(tif, frame(), decide_filter(nx), deflate_level);
			printf(" Frame %3d / %3d\n", iframe + 1, nn);
		}

		TIFFClose(tif);
		std::rename(fn_tmp.c_str(), fn_tiff.c_str());
	}

	int checkMRCtype(FileName fn_movie)
	{
		// Check data type; Unfortunately I cannot do this through Image object.
		FILE *mrcin = fopen(fn_movie.c_str(), "r");
		int headers[25];
		fread(headers, sizeof(int), 24, mrcin);
		fclose(mrcin);

		return headers[3];
	}

	void initialise()
	{
		FileName fn_first;

		if (fn_in.getExtension() == "star")
		{
			MD.read(fn_in, "movie");

			// Support non-optics group STAR files
			if (MD.numberOfObjects() == 0)
				MD.read(fn_in, "");

			if (!MD.getValue(EMDL_MICROGRAPH_MOVIE_NAME, fn_first, 0))
				REPORT_ERROR("The input STAR file does not contain the rlnMicrographMovieName column");

			std::cout << "The number of movies in the input: " << MD.numberOfObjects() << std::endl;
		}
		else
		{
			MD.addObject();
			MD.setValue(EMDL_MICROGRAPH_MOVIE_NAME, fn_in);
			fn_first = fn_in;
		}
		
		// Check type and mode of the input
		Image<RFLOAT> Ihead;
		Ihead.read(fn_first, false, -1, false, true); // select_img -1, mmap false, is_2D true
		nn = NSIZE(Ihead());
		ny = YSIZE(Ihead());
		nx = XSIZE(Ihead());
		mrc_mode = checkMRCtype(fn_first);
		printf("Input (NX, NY, NN) = (%d, %d, %d), MODE = %d\n\n", nx, ny, nn, mrc_mode);

		if (fn_gain != "")
		{
			if (mrc_mode != 2)
			{
				std::cerr << "The input movie is not in mode 2. A gain reference is irrelavant." << std::endl;
			}
			else
			{
				gain.read(fn_gain + ":mrc");
				std::cout << "Read " << fn_gain << std::endl;
				if (XSIZE(gain()) != nx || YSIZE(gain()) != ny)
					REPORT_ERROR("The input gain has a wrong size.");

				FileName fn_defects = fn_gain.withoutExtension() + "_reliablity." + fn_gain.getExtension();
				defects.read(fn_defects + ":mrc");
				std::cout << "Read " << fn_defects << "\n" << std::endl;
				if (XSIZE(defects()) != nx || YSIZE(defects()) != ny)
					REPORT_ERROR("The input reliability map has a wrong size.");
			}
		}
		else if (mrc_mode == 2)
		{
			gain().reshape(ny, nx);
			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(gain())
				DIRECT_MULTIDIM_ELEM(gain(), n) = 999.9;
			defects().reshape(ny, nx);
			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(defects())
				DIRECT_MULTIDIM_ELEM(defects(), n) = -1;
		}

		if (fn_out.contains("/"))
			system(("mkdir -p " + fn_out.beforeLastOf("/")).c_str());

		if (!do_estimate && mrc_mode == 2)
		{
			// TODO: other strategy
			FOR_ALL_DIRECT_ELEMENTS_IN_MULTIDIMARRAY(gain())
				if (DIRECT_MULTIDIM_ELEM(defects(), n) < thresh_reliable)
					DIRECT_MULTIDIM_ELEM(gain(), n) = 1.0;

			gain.write(fn_out + "gain-reference.mrc");
			std::cout << "Written " + fn_out + "gain-reference.mrc. Please use this file as a gain reference when processing the converted movies.\n" << std::endl; 
		}
	}

	void processOneMovie(FileName fn_movie, FileName fn_tiff)
	{
		// Check type and mode of the input
		Image<RFLOAT> Ihead;
		Ihead.read(fn_movie, false, -1, false, true); // select_img -1, mmap false, is_2D true
		if (ny != YSIZE(Ihead()) || nx != XSIZE(Ihead()) || mrc_mode != checkMRCtype(fn_movie))
			REPORT_ERROR("A movie " + fn_movie + " has a different size and/or mode from other movies.");

		if (mrc_mode == 1 || mrc_mode == 6)
		{
			only_compress<short>(fn_movie, fn_tiff);
		}
		else if (mrc_mode == 0 || mrc_mode == 101)
		{
			only_compress<char>(fn_movie, fn_tiff);
		}

		if (do_estimate)
		{
			estimate(fn_in);

			// Write for each movie so that one can stop anytime
			gain.write(fn_out + "gain_estimate.bin:mrc"); // .bin to prevent people from using this by mistake
			defects.write(fn_out + "gain_estimate_reliablity.bin:mrc");

			std::cout << "\nUpdated " + fn_out + "gain_estimate.bin and " + fn_out + "gain_estimate_reliablity.bin\n" << std::endl;
		}
		else
		{
			if (only_do_unfinished && exists(fn_tiff))
				return;

			unnormalise<float>(fn_movie, fn_tiff);
		}
	}

	void run()
	{
		initialise();

		long int my_first = 0, my_last = MD.numberOfObjects() - 1; // ?
		// divide_equally(MD.numberOfParticles(), size, rank, my_first, my_last); // MPI parallelization

		for (long i = my_first; i <= my_last; i++)
		{
			FileName fn_movie, fn_tiff;
			MD.getValue(EMDL_MICROGRAPH_MOVIE_NAME, fn_movie, i);

			fn_tiff = fn_out + fn_movie.withoutExtension() + ".tif";
			std::cout << "Processing " << fn_movie;
			if (!do_estimate)
				std::cout  << " into " << fn_tiff;
			std::cout << std::endl;

			if (fn_tiff.contains("/"))
				system(("mkdir -p " + fn_tiff.beforeLastOf("/")).c_str());

			processOneMovie(fn_movie, fn_tiff);
		}
	}
};

int main(int argc, char *argv[])
{
	convert_to_tiff app;

	try
	{
		app.read(argc, argv);
		app.run();
	}
	catch (RelionError XE)
	{
        	std::cerr << XE;
	        return RELION_EXIT_FAILURE;
	}

	return RELION_EXIT_SUCCESS;
}
#endif
