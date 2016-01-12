/******************************************************************
Copyright 2006 by Michael Farrar.  All rights reserved.
This program may not be sold or incorporated into a commercial product,
in whole or in part, without written consent of Michael Farrar.
*******************************************************************/

/*
   Written by Michael Farrar, 2006 (alignment), Mengyao Zhao (SSW Library) and Martin Steinegger (change structure add aa composition, profile and AVX2 support).
   Please send bug reports and/or suggestions to martin.steinegger@campus.lmu.de.
*/
#include "smith_waterman_sse2.h"

#include <Sequence.h>
#include <simd.h>
#include <Util.h>
#include <BaseMatrix.h>
#include <SubstitutionMatrix.h>

SmithWaterman::SmithWaterman(size_t maxSequenceLength, int aaSize, bool aaBiasCorrection) {
	maxSequenceLength += 1;
	this->aaBiasCorrection = aaBiasCorrection;
	const int segSize = (maxSequenceLength+7)/8;
	vHStore = (simd_int*) mem_align(ALIGN_INT, segSize * sizeof(simd_int));
	vHLoad  = (simd_int*) mem_align(ALIGN_INT, segSize * sizeof(simd_int));
	vE      = (simd_int*) mem_align(ALIGN_INT, segSize * sizeof(simd_int));
	vHmax   = (simd_int*) mem_align(ALIGN_INT, segSize * sizeof(simd_int));
	profile = new s_profile();
	profile->profile_byte = (simd_int*)mem_align(ALIGN_INT, aaSize * segSize * sizeof(simd_int));
	profile->profile_word = (simd_int*)mem_align(ALIGN_INT, aaSize * segSize * sizeof(simd_int));
	profile->profile_rev_byte = (simd_int*)mem_align(ALIGN_INT, aaSize * segSize * sizeof(simd_int));
	profile->profile_rev_word = (simd_int*)mem_align(ALIGN_INT, aaSize * segSize * sizeof(simd_int));
	profile->query_rev_sequence = new int8_t[maxSequenceLength];
	profile->query_sequence     = new int8_t[maxSequenceLength];
	profile->composition_bias   = new int8_t[maxSequenceLength];
	profile->composition_bias_rev   = new int8_t[maxSequenceLength];
	profile->profile_word_linear = new short*[aaSize];
	profile_word_linear_data = new short[aaSize*maxSequenceLength];
	profile->mat_rev            = new int8_t[maxSequenceLength * aaSize * 2];
	profile->mat                = new int8_t[maxSequenceLength * aaSize * 2];
	tmp_composition_bias   = new float[maxSequenceLength];

	memset(profile->query_sequence, 0, maxSequenceLength * sizeof(int8_t));
	memset(profile->query_rev_sequence, 0, maxSequenceLength * sizeof(int8_t));
	memset(profile->mat_rev, 0, maxSequenceLength * Sequence::PROFILE_AA_SIZE);
	memset(profile->composition_bias, 0, maxSequenceLength * sizeof(int8_t));
	memset(profile->composition_bias_rev, 0, maxSequenceLength * sizeof(int8_t));
	/* array to record the largest score of each reference position */
	maxColumn = new uint8_t[maxSequenceLength*sizeof(uint16_t)];
	memset(maxColumn, 0, maxSequenceLength*sizeof(uint16_t));
	workspace = new scores[maxSequenceLength * 2  + 2];
	memset(workspace, 0, sizeof(scores) * 2 + 2 * sizeof(scores));
	btMatrix = new unsigned char[(maxSequenceLength*maxSequenceLength)/4];
}

SmithWaterman::~SmithWaterman(){
	free(vHStore);
	free(vHmax);
	free(vE);
	free(vHLoad);
	free(profile->profile_byte);
	free(profile->profile_word);
	free(profile->profile_rev_byte);
	free(profile->profile_rev_word);

	delete [] profile->query_rev_sequence;
	delete [] profile->composition_bias;
	delete [] profile->composition_bias_rev;
	delete [] profile->profile_word_linear;
	delete [] profile_word_linear_data;
	delete [] tmp_composition_bias;
	delete [] profile->query_sequence;
	delete [] profile->mat_rev;
	delete [] profile->mat;
	delete profile;
	delete [] maxColumn;
	delete [] workspace;
	delete [] btMatrix;
}


/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch. */
template <typename T, size_t Elements, const unsigned int type>
void SmithWaterman::createQueryProfile(simd_int *profile, const int8_t *query_sequence, const int8_t * composition_bias, const int8_t *mat,
									   const int32_t query_length, const int32_t aaSize, uint8_t bias,
									   const int32_t offset, const int32_t entryLength) {

	const int32_t segLen = (query_length+Elements-1)/Elements;
	T* t = (T*)profile;

	/* Generate query profile rearrange query sequence & calculate the weight of match/mismatch */
	for (int32_t nt = 0; LIKELY(nt < aaSize); nt++) {
//		printf("{");
		for (int32_t i = 0; i < segLen; i ++) {
			int32_t  j = i;
//			printf("(");
			for (size_t segNum = 0; LIKELY(segNum < Elements) ; segNum ++) {
				// if will be optmized out by compiler
				if(type == SUBSTITUTIONMATRIX) {     // substitution score for query_seq constrained by nt
					// query_sequence starts from 1 to n
					*t++ = ( j >= query_length) ? bias : mat[nt * aaSize + query_sequence[j + offset ]] + composition_bias[j + offset] + bias; // mat[nt][q[j]] mat eq 20*20
//					printf("(%1d, %1d) ", query_sequence[j ], *(t-1));

				} if(type == PROFILE) {
					// profile starts by 0
					*t++ = ( j >= query_length) ? bias : mat[nt * entryLength  + (j + (offset - 1) )] + bias; //mat eq L*20  // mat[nt][j]
//					printf("(%1d, %1d) ", j , *(t-1));
				}
				j += segLen;
			}
//			printf(")");
		}
//		printf("}\n");
	}
//	printf("\n");
//	std::flush(std::cout);

}


s_align* SmithWaterman::ssw_align (
		const int *db_sequence,
		int32_t db_length,
		const uint8_t gap_open,
		const uint8_t gap_extend,
		const uint8_t flag,	//  (from high to low) bit 5: return the best alignment beginning position; 6: if (ref_end1 - ref_begin1 <= filterd) && (read_end1 - read_begin1 <= filterd), return cigar; 7: if max score >= filters, return cigar; 8: always return cigar; if 6 & 7 are both setted, only return cigar when both filter fulfilled
		const uint16_t filters,
		const int32_t filterd,
		const int32_t maskLen) {

	alignment_end* bests = 0, *bests_reverse = 0;
	int32_t word = 0, query_length = profile->query_length;
	cigar* path;
	s_align* r = new s_align;
	r->dbStartPos1 = -1;
	r->qStartPos1 = -1;
	r->cigar = 0;
	r->cigarLen = 0;
	//if (maskLen < 15) {
	//	fprintf(stderr, "When maskLen < 15, the function ssw_align doesn't return 2nd best alignment information.\n");
	//}

	// Find the alignment scores and ending positions
	if (profile->profile_byte) {
		bests = sw_sse2_byte(db_sequence, 0, db_length, query_length, gap_open, gap_extend, profile->profile_byte, -1, profile->bias, maskLen);

		if (profile->profile_word && bests[0].score == 255) {
			free(bests);
			bests = sw_sse2_word(db_sequence, 0, db_length, query_length, gap_open, gap_extend, profile->profile_word, -1, maskLen);
			word = 1;
		} else if (bests[0].score == 255) {
			fprintf(stderr, "Please set 2 to the score_size parameter of the function ssw_init, otherwise the alignment results will be incorrect.\n");
			delete r;
			return NULL;
		}
	}else if (profile->profile_word) {
		bests = sw_sse2_word(db_sequence, 0, db_length, query_length, gap_open, gap_extend, profile->profile_word, -1, maskLen);
		word = 1;
	}else {
		fprintf(stderr, "Please call the function ssw_init before ssw_align.\n");
		delete r;
		return NULL;
	}
	r->score1 = bests[0].score;
	r->dbEndPos1 = bests[0].ref;
	r->qEndPos1 = bests[0].read;
	if (maskLen >= 15) {
		r->score2 = bests[1].score;
		r->ref_end2 = bests[1].ref;
	} else {
		r->score2 = 0;
		r->ref_end2 = -1;
	}
	free(bests);
	int32_t queryOffset = query_length - r->qEndPos1;

	if (flag == 0 || ((flag == 2 || flag == 1) && r->score1 < filters)){
		goto end;
	}

	// Find the beginning position of the best alignment.
	if (word == 0) {
		if(profile->sequence_type == Sequence::HMM_PROFILE){
			createQueryProfile<int8_t, VECSIZE_INT * 4, PROFILE>(profile->profile_rev_byte, profile->query_rev_sequence, NULL, profile->mat_rev,
																 r->qEndPos1 + 1, profile->alphabetSize, profile->bias, queryOffset, profile->query_length);
		}else{
			createQueryProfile<int8_t, VECSIZE_INT * 4, SUBSTITUTIONMATRIX>(profile->profile_rev_byte, profile->query_rev_sequence, profile->composition_bias_rev, profile->mat,
																			r->qEndPos1 + 1, profile->alphabetSize, profile->bias, queryOffset, 0);
		}
		bests_reverse = sw_sse2_byte(db_sequence, 1, r->dbEndPos1 + 1, r->qEndPos1 + 1, gap_open, gap_extend, profile->profile_rev_byte,
									 r->score1, profile->bias, maskLen);
	} else {
		if(profile->sequence_type == Sequence::HMM_PROFILE) {
			createQueryProfile<int16_t, VECSIZE_INT * 2, PROFILE>(profile->profile_rev_word, profile->query_rev_sequence, NULL, profile->mat_rev,
																  r->qEndPos1 + 1, profile->alphabetSize, 0, queryOffset, profile->query_length);

		}else{
			createQueryProfile<int16_t, VECSIZE_INT * 2, SUBSTITUTIONMATRIX>(profile->profile_rev_word, profile->query_rev_sequence, profile->composition_bias_rev, profile->mat,
																			 r->qEndPos1 + 1, profile->alphabetSize, 0, queryOffset, 0);
		}
		bests_reverse = sw_sse2_word(db_sequence, 1, r->dbEndPos1 + 1, r->qEndPos1 + 1, gap_open, gap_extend, profile->profile_rev_word,
									 r->score1, maskLen);
	}
	if(bests_reverse->score != r->score1){
		fprintf(stderr, "Score of forward/backward SW differ. This should not happen.\n");
		delete r;
		return NULL;
	}

	r->dbStartPos1 = bests_reverse[0].ref;
	r->qStartPos1 = r->qEndPos1 - bests_reverse[0].read;

	free(bests_reverse);
	if (flag == 1) // just start and end point are needed
		goto end;

	// Generate cigar.
	path = banded_sw(db_sequence,(const short **) profile->profile_word_linear, r->qStartPos1, r->qEndPos1 + 1,
					 r->dbStartPos1, r->dbEndPos1 + 1, gap_open, gap_extend);

	if (path == 0) {
		delete r;
		r = NULL;
	}
	else {
		r->cigar = path->seq;
		r->cigarLen = path->length;
	}	delete(path);


	end:
	return r;
}


static void seq_reverse(int8_t * reverse, const int8_t* seq, int32_t end)	/* end is 0-based alignment ending position */
{
	int32_t start = 0;
	while (LIKELY(start <= end)) {
		reverse[start] = seq[end];
		reverse[end] = seq[start];
		++start;
		--end;
	}
}


char SmithWaterman::cigar_int_to_op (uint32_t cigar_int)
{
	uint8_t letter_code = cigar_int & 0xfU;
	static const char map[] = {
			'M',
			'I',
			'D',
			'N',
			'S',
			'H',
			'P',
			'=',
			'X',
	};

	if (letter_code >= (sizeof(map)/sizeof(map[0]))) {
		return 'M';
	}

	return map[letter_code];
}

uint32_t SmithWaterman::cigar_int_to_len (uint32_t cigar_int)
{
	uint32_t res = cigar_int >> 4;
	return res;
}

SmithWaterman::alignment_end* SmithWaterman::sw_sse2_byte (const int* db_sequence,
														   int8_t ref_dir,	// 0: forward ref; 1: reverse ref
														   int32_t db_length,
														   int32_t query_lenght,
														   const uint8_t gap_open, /* will be used as - */
														   const uint8_t gap_extend, /* will be used as - */
														   const simd_int* query_profile_byte,
														   uint8_t terminate,	/* the best alignment score: used to terminate
                                                         the matrix calculation when locating the
                                                         alignment beginning point. If this score
                                                         is set to 0, it will not be used */
														   uint8_t bias,  /* Shift 0 point to a positive value. */
														   int32_t maskLen) {
#define max16(m, vm) ((m) = simdi8_hmax((vm)));

	uint8_t max = 0;		                     /* the max alignment score */
	int32_t end_query = query_lenght - 1;
	int32_t end_db = -1; /* 0_based best alignment ending point; Initialized as isn't aligned -1. */
	const int SIMD_SIZE = VECSIZE_INT * 4;
	int32_t segLen = (query_lenght + SIMD_SIZE-1) / SIMD_SIZE; /* number of segment */
	/* array to record the largest score of each reference position */
	memset(this->maxColumn, 0, db_length * sizeof(uint8_t));
	uint8_t * maxColumn = (uint8_t *) this->maxColumn;

	/* Define 16 byte 0 vector. */
	simd_int vZero = simdi32_set(0);
	simd_int* pvHStore = vHStore;
	simd_int* pvHLoad = vHLoad;
	simd_int* pvE = vE;
	simd_int* pvHmax = vHmax;
	memset(pvHStore,0,segLen*sizeof(simd_int));
	memset(pvHLoad,0,segLen*sizeof(simd_int));
	memset(pvE,0,segLen*sizeof(simd_int));
	memset(pvHmax,0,segLen*sizeof(simd_int));

	int32_t i, j;
	/* 16 byte insertion begin vector */
	simd_int vGapO = simdi8_set(gap_open);

	/* 16 byte insertion extension vector */
	simd_int vGapE = simdi8_set(gap_extend);

	/* 16 byte bias vector */
	simd_int vBias = simdi8_set(bias);

	simd_int vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	simd_int vMaxMark = vZero; /* Trace the highest score till the previous column. */
	simd_int vTemp;
	int32_t edge, begin = 0, end = db_length, step = 1;
	//	int32_t distance = query_lenght * 2 / 3;
	//	int32_t distance = query_lenght / 2;
	//	int32_t distance = query_lenght;

	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = db_length - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		simd_int e, vF = vZero, vMaxColumn = vZero; /* Initialize F value to 0.
                                                    Any errors to vH values will be corrected in the Lazy_F loop.
                                                    */
		//		max16(maxColumn[i], vMaxColumn);
		//		fprintf(stderr, "middle[%d]: %d\n", i, maxColumn[i]);

		simd_int vH = pvHStore[segLen - 1];
		vH = simdi8_shiftl (vH, 1); /* Shift the 128-bit value in vH left by 1 byte. */
		const simd_int* vP = query_profile_byte + db_sequence[i] * segLen; /* Right part of the query_profile_byte */
		//	int8_t* t;
		//	int32_t ti;
		//        fprintf(stderr, "i: %d of %d:\t ", i,segLen);
		//for (t = (int8_t*)vP, ti = 0; ti < segLen; ++ti) fprintf(stderr, "%d\t", *t++);
		//fprintf(stderr, "\n");

		/* Swap the 2 H buffers. */
		simd_int* pv = pvHLoad;
		pvHLoad = pvHStore;
		pvHStore = pv;

		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); ++j) {
			vH = simdui8_adds(vH, simdi_load(vP + j));
			vH = simdui8_subs(vH, vBias); /* vH will be always > 0 */
			//	max16(maxColumn[i], vH);
			//	fprintf(stderr, "H[%d]: %d\n", i, maxColumn[i]);
			//	int8_t* t;
			//	int32_t ti;
			//for (t = (int8_t*)&vH, ti = 0; ti < 16; ++ti) fprintf(stderr, "%d\t", *t++);

			/* Get max from vH, vE and vF. */
			e = simdi_load(pvE + j);
			vH = simdui8_max(vH, e);
			vH = simdui8_max(vH, vF);
			vMaxColumn = simdui8_max(vMaxColumn, vH);

			//	max16(maxColumn[i], vMaxColumn);
			//	fprintf(stderr, "middle[%d]: %d\n", i, maxColumn[i]);
			//	for (t = (int8_t*)&vMaxColumn, ti = 0; ti < 16; ++ti) fprintf(stderr, "%d\t", *t++);

			/* Save vH values. */
			simdi_store(pvHStore + j, vH);

			/* Update vE value. */
			vH = simdui8_subs(vH, vGapO); /* saturation arithmetic, result >= 0 */
			e = simdui8_subs(e, vGapE);
			e = simdui8_max(e, vH);
			simdi_store(pvE + j, e);

			/* Update vF value. */
			vF = simdui8_subs(vF, vGapE);
			vF = simdui8_max(vF, vH);

			/* Load the next vH. */
			vH = simdi_load(pvHLoad + j);
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		/* reset pointers to the start of the saved data */
		j = 0;
		vH = simdi_load (pvHStore + j);

		/*  the computed vF value is for the given column.  since */
		/*  we are at the end, we need to shift the vF value over */
		/*  to the next column. */
		vF = simdi8_shiftl (vF, 1);
		vTemp = simdui8_subs (vH, vGapO);
		vTemp = simdui8_subs (vF, vTemp);
		vTemp = simdi8_eq (vTemp, vZero);
		cmp  = simdi8_movemask (vTemp);
#ifdef AVX2
		while (cmp != 0xffffffff)
#else
			while (cmp != 0xffff)
#endif
		{
			vH = simdui8_max (vH, vF);
			vMaxColumn = simdui8_max(vMaxColumn, vH);
			simdi_store (pvHStore + j, vH);
			vF = simdui8_subs (vF, vGapE);
			j++;
			if (j >= segLen)
			{
				j = 0;
				vF = simdi8_shiftl (vF, 1);
			}
			vH = simdi_load (pvHStore + j);

			vTemp = simdui8_subs (vH, vGapO);
			vTemp = simdui8_subs (vF, vTemp);
			vTemp = simdi8_eq (vTemp, vZero);
			cmp  = simdi8_movemask (vTemp);
		}

		vMaxScore = simdui8_max(vMaxScore, vMaxColumn);
		vTemp = simdi8_eq(vMaxMark, vMaxScore);
		cmp = simdi8_movemask(vTemp);
#ifdef AVX2
		if (cmp != 0xffffffff)
#else
			if (cmp != 0xffff)
#endif
		{
			uint8_t temp;
			vMaxMark = vMaxScore;
			max16(temp, vMaxScore);
			vMaxScore = vMaxMark;

			if (LIKELY(temp > max)) {
				max = temp;
				if (max + bias >= 255) break;	//overflow
				end_db = i;

				/* Store the column with the highest alignment score in order to trace the alignment ending position on read. */
				for (j = 0; LIKELY(j < segLen); ++j) pvHmax[j] = pvHStore[j];
			}
		}

		/* Record the max score of current column. */
		max16(maxColumn[i], vMaxColumn);
		//		fprintf(stderr, "maxColumn[%d]: %d\n", i, maxColumn[i]);
		if (maxColumn[i] == terminate) break;
	}

	/* Trace the alignment ending position on read. */
	uint8_t *t = (uint8_t*)pvHmax;
	int32_t column_len = segLen * SIMD_SIZE;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		int32_t temp;
		if (*t == max) {
			temp = i / SIMD_SIZE + i % SIMD_SIZE * segLen;
			if (temp < end_query) end_query = temp;
		}
	}

	/* Find the most possible 2nd best alignment. */
	alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max + bias >= 255 ? 255 : max;
	bests[0].ref = end_db;
	bests[0].read = end_query;

	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;

	edge = (end_db - maskLen) > 0 ? (end_db - maskLen) : 0;
	for (i = 0; i < edge; i ++) {
		//			fprintf (stderr, "maxColumn[%d]: %d\n", i, maxColumn[i]);
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}
	edge = (end_db + maskLen) > db_length ? db_length : (end_db + maskLen);
	for (i = edge + 1; i < db_length; i ++) {
		//			fprintf (stderr, "db_length: %d\tmaxColumn[%d]: %d\n", db_length, i, maxColumn[i]);
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}

	return bests;
#undef max16
}


SmithWaterman::alignment_end* SmithWaterman::sw_sse2_word (const int* db_sequence,
														   int8_t ref_dir,	// 0: forward ref; 1: reverse ref
														   int32_t db_length,
														   int32_t query_lenght,
														   const uint8_t gap_open, /* will be used as - */
														   const uint8_t gap_extend, /* will be used as - */
														   const simd_int*query_profile_word,
														   uint16_t terminate,
														   int32_t maskLen) {

#define max8(m, vm) ((m) = simdi16_hmax((vm)));

	uint16_t max = 0;		                     /* the max alignment score */
	int32_t end_read = query_lenght - 1;
	int32_t end_ref = 0; /* 1_based best alignment ending point; Initialized as isn't aligned - 0. */
	const int32_t SIMD_SIZE = VECSIZE_INT * 2;
	int32_t segLen = (query_lenght + SIMD_SIZE-1) / SIMD_SIZE; /* number of segment */
	/* array to record the alignment read ending position of the largest score of each reference position */
	memset(this->maxColumn, 0, db_length * sizeof(uint16_t));
	uint16_t * maxColumn = (uint16_t *) this->maxColumn;

	/* Define 16 byte 0 vector. */
	simd_int vZero = simdi32_set(0);
	simd_int* pvHStore = vHStore;
	simd_int* pvHLoad = vHLoad;
	simd_int* pvE = vE;
	simd_int* pvHmax = vHmax;
	memset(pvHStore,0,segLen*sizeof(simd_int));
	memset(pvHLoad,0, segLen*sizeof(simd_int));
	memset(pvE,0,     segLen*sizeof(simd_int));
	memset(pvHmax,0,  segLen*sizeof(simd_int));

	int32_t i, j, k;
	/* 16 byte insertion begin vector */
	simd_int vGapO = simdi16_set(gap_open);

	/* 16 byte insertion extension vector */
	simd_int vGapE = simdi16_set(gap_extend);

	simd_int vMaxScore = vZero; /* Trace the highest score of the whole SW matrix. */
	simd_int vMaxMark = vZero; /* Trace the highest score till the previous column. */
	simd_int vTemp;
	int32_t edge, begin = 0, end = db_length, step = 1;

	/* outer loop to process the reference sequence */
	if (ref_dir == 1) {
		begin = db_length - 1;
		end = -1;
		step = -1;
	}
	for (i = begin; LIKELY(i != end); i += step) {
		int32_t cmp;
		simd_int e, vF = vZero; /* Initialize F value to 0.
                                Any errors to vH values will be corrected in the Lazy_F loop.
                                */
		simd_int vH = pvHStore[segLen - 1];
		vH = simdi8_shiftl (vH, 2); /* Shift the 128-bit value in vH left by 2 byte. */

		/* Swap the 2 H buffers. */
		simd_int* pv = pvHLoad;

		simd_int vMaxColumn = vZero; /* vMaxColumn is used to record the max values of column i. */

		const simd_int* vP = query_profile_word + db_sequence[i] * segLen; /* Right part of the query_profile_byte */
		pvHLoad = pvHStore;
		pvHStore = pv;

		/* inner loop to process the query sequence */
		for (j = 0; LIKELY(j < segLen); j ++) {
			vH = simdi16_adds(vH, simdi_load(vP + j));

			/* Get max from vH, vE and vF. */
			e = simdi_load(pvE + j);
			vH = simdi16_max(vH, e);
			vH = simdi16_max(vH, vF);
			vMaxColumn = simdi16_max(vMaxColumn, vH);

			/* Save vH values. */
			simdi_store(pvHStore + j, vH);

			/* Update vE value. */
			vH = simdui16_subs(vH, vGapO); /* saturation arithmetic, result >= 0 */
			e = simdui16_subs(e, vGapE);
			e = simdi16_max(e, vH);
			simdi_store(pvE + j, e);

			/* Update vF value. */
			vF = simdui16_subs(vF, vGapE);
			vF = simdi16_max(vF, vH);

			/* Load the next vH. */
			vH = simdi_load(pvHLoad + j);
		}

		/* Lazy_F loop: has been revised to disallow adjecent insertion and then deletion, so don't update E(i, j), learn from SWPS3 */
		for (k = 0; LIKELY(k < SIMD_SIZE); ++k) {
			vF = simdi8_shiftl (vF, 2);
			for (j = 0; LIKELY(j < segLen); ++j) {
				vH = simdi_load(pvHStore + j);
				vH = simdi16_max(vH, vF);
				simdi_store(pvHStore + j, vH);
				vH = simdui16_subs(vH, vGapO);
				vF = simdui16_subs(vF, vGapE);
				if (UNLIKELY(! simdi8_movemask(simdi16_gt(vF, vH)))) goto end;
			}
		}

		end:
		vMaxScore = simdi16_max(vMaxScore, vMaxColumn);
		vTemp = simdi16_eq(vMaxMark, vMaxScore);
		cmp = simdi8_movemask(vTemp);

#ifdef AVX2
		if (cmp != 0xffffffff)
#else
			if (cmp != 0xffff)
#endif
		{
			uint16_t temp;
			vMaxMark = vMaxScore;
			max8(temp, vMaxScore);
			vMaxScore = vMaxMark;

			if (LIKELY(temp > max)) {
				max = temp;
				end_ref = i;
				for (j = 0; LIKELY(j < segLen); ++j) pvHmax[j] = pvHStore[j];
			}
		}

		/* Record the max score of current column. */
		max8(maxColumn[i], vMaxColumn);
		if (maxColumn[i] == terminate) break;
	}

	/* Trace the alignment ending position on read. */
	uint16_t *t = (uint16_t*)pvHmax;
	int32_t column_len = segLen * SIMD_SIZE;
	for (i = 0; LIKELY(i < column_len); ++i, ++t) {
		int32_t temp;
		if (*t == max) {
			temp = i / SIMD_SIZE + i % SIMD_SIZE * segLen;
			if (temp < end_read) end_read = temp;
		}
	}

	/* Find the most possible 2nd best alignment. */
	SmithWaterman::alignment_end* bests = (alignment_end*) calloc(2, sizeof(alignment_end));
	bests[0].score = max;
	bests[0].ref = end_ref;
	bests[0].read = end_read;

	bests[1].score = 0;
	bests[1].ref = 0;
	bests[1].read = 0;

	edge = (end_ref - maskLen) > 0 ? (end_ref - maskLen) : 0;
	for (i = 0; i < edge; i ++) {
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}
	edge = (end_ref + maskLen) > db_length ? db_length : (end_ref + maskLen);
	for (i = edge; i < db_length; i ++) {
		if (maxColumn[i] > bests[1].score) {
			bests[1].score = maxColumn[i];
			bests[1].ref = i;
		}
	}

	return bests;
#undef max8
}

void SmithWaterman::ssw_init (const Sequence* q,
							  const int8_t* mat,
							  const BaseMatrix *m,
							  const int32_t alphabetSize,
							  const int8_t score_size) {

	profile->bias = 0;
	profile->sequence_type = q->getSequenceType();
	int32_t compositionBias = 0;
	if(aaBiasCorrection == true) {
		SubstitutionMatrix::calcLocalAaBiasCorrection(m, q->int_sequence, q->L, tmp_composition_bias);
		for(int i =0; i < q->L; i++){
			profile->composition_bias[i] = (int8_t) (tmp_composition_bias[i] < 0.0)? tmp_composition_bias[i] - 0.5: tmp_composition_bias[i] + 0.5;
			compositionBias = (static_cast<int8_t>(compositionBias) < profile->composition_bias[i])
							   ? compositionBias  :  profile->composition_bias[i];
		}
		compositionBias = std::min(compositionBias, 0);
//		std::cout << compositionBias << std::endl;
	}else{
		memset(profile->composition_bias, 0, q->L* sizeof(int8_t));
	}
	// copy memory to local memory
	if(profile->sequence_type == Sequence::HMM_PROFILE ){
		memcpy(profile->mat, mat, q->L * Sequence::PROFILE_AA_SIZE * sizeof(int8_t));
		// set neutral state 'X' (score=0)
		memset(profile->mat + ((alphabetSize - 1) * q->L), 0, q->L * sizeof(int8_t ));
	}else{
		memcpy(profile->mat, mat, alphabetSize * alphabetSize * sizeof(int8_t));
	}
	for(int i = 0; i < q->L; i++){
		profile->query_sequence[i] = (int8_t) q->int_sequence[i];
	}
	if (score_size == 0 || score_size == 2) {
		/* Find the bias to use in the substitution matrix */
		int32_t bias = 0;
		int32_t matSize =  alphabetSize * alphabetSize;
		if(q->getSequenceType() == Sequence::HMM_PROFILE) {
			matSize = q->L * Sequence::PROFILE_AA_SIZE;
		}
		for (int32_t i = 0; i < matSize; i++){
			if (mat[i] < bias){
				bias = mat[i];
			}
		}
		bias = abs(bias) + abs(compositionBias);

		profile->bias = bias;
		if(q->getSequenceType() == Sequence::HMM_PROFILE){
			createQueryProfile<int8_t, VECSIZE_INT * 4, PROFILE>(profile->profile_byte, profile->query_sequence, NULL, profile->mat, q->L, alphabetSize, bias, 1, q->L);
		}else{
			createQueryProfile<int8_t, VECSIZE_INT * 4, SUBSTITUTIONMATRIX>(profile->profile_byte, profile->query_sequence, profile->composition_bias, profile->mat, q->L, alphabetSize, bias, 0, 0);
		}
	}
	if (score_size == 1 || score_size == 2) {
		if(q->getSequenceType() == Sequence::HMM_PROFILE){
			createQueryProfile<int16_t, VECSIZE_INT * 2, PROFILE>(profile->profile_word, profile->query_sequence, NULL, profile->mat, q->L, alphabetSize, 0, 1, q->L);
			for(int32_t i = 0; i< alphabetSize; i++) {
				profile->profile_word_linear[i] = &profile_word_linear_data[i*q->L];
				for (int j = 0; j < q->L; j++) {
					profile->profile_word_linear[i][j] = mat[i * q->L + q->int_sequence[j]];
				}
			}
		}else{
			createQueryProfile<int16_t, VECSIZE_INT * 2, SUBSTITUTIONMATRIX>(profile->profile_word, profile->query_sequence, profile->composition_bias, profile->mat, q->L, alphabetSize, 0, 0, 0);
			for(int32_t i = 0; i< alphabetSize; i++) {
				profile->profile_word_linear[i] = &profile_word_linear_data[i*q->L];
				for (int j = 0; j < q->L; j++) {
					profile->profile_word_linear[i][j] = mat[i * alphabetSize + q->int_sequence[j]] + profile->composition_bias[j];
				}
			}
		}


	}
	// create reverse structures
	seq_reverse( profile->query_rev_sequence, profile->query_sequence, q->L);
	seq_reverse( profile->composition_bias_rev, profile->composition_bias, q->L);

	if(q->getSequenceType() == Sequence::HMM_PROFILE) {
		for (int32_t i = 0; i < alphabetSize; i++) {
			const int8_t *startToRead = profile->mat + (i * q->L);
			int8_t *startToWrite      = profile->mat_rev + (i * q->L);
			std::reverse_copy(startToRead, startToRead + q->L, startToWrite);
		}
	}
	profile->query_length = q->L;
	profile->alphabetSize = alphabetSize;

}
SmithWaterman::cigar * SmithWaterman::banded_sw(
		const int *db_sequence,
		const short ** profile_word,
		int32_t query_start, int32_t query_end,
		int32_t target_start, int32_t target_end,
		const short gap_open, const short gap_extend)
{
	const int M = 2;
	const int F = 1;
	const int E = 0;
	SmithWaterman::cigar * result = new SmithWaterman::cigar;
	scores * curr_sM_G_D_vec = &workspace[0];
	int query_length = (query_end - query_start);
	int target_length = (target_end - target_start);
	scores * prev_sM_G_D_vec = &workspace[query_length + 1];
	memset(prev_sM_G_D_vec, 0, sizeof(scores) * (query_end + 1));
	short goe = gap_open + gap_extend;
	int pos = 0;
	memset(btMatrix , 0, target_length * (query_length/4 +3) * sizeof(unsigned char));

	for (int i = target_start; LIKELY(i < target_end); i++) {
		prev_sM_G_D_vec[query_start].H = 0;
		curr_sM_G_D_vec[query_start].H = 0;
		curr_sM_G_D_vec[query_start].E = 0;
		const short * profile = profile_word[db_sequence[i]];
		for (int j = query_start+1; LIKELY(j <= query_end); j++) {
			curr_sM_G_D_vec[j].E = std::max(curr_sM_G_D_vec[j-1].H - goe, curr_sM_G_D_vec[j-1].E - gap_extend); // j-1
			curr_sM_G_D_vec[j].F = std::max(prev_sM_G_D_vec[j].H   - goe, prev_sM_G_D_vec[j].F - gap_extend);   // i-1
			const short H = prev_sM_G_D_vec[j-1].H + profile[j-1]; // i - 1, j - 1
			curr_sM_G_D_vec[j].H = std::max(H, curr_sM_G_D_vec[j].E);
			curr_sM_G_D_vec[j].H = std::max(curr_sM_G_D_vec[j].H, curr_sM_G_D_vec[j].F);
			const unsigned char mode1 = (curr_sM_G_D_vec[j].H == H) ? M : E;
			const unsigned char mode2 = (curr_sM_G_D_vec[j].H == curr_sM_G_D_vec[j].F) ? F : E;
			const unsigned char mode = std::max(mode1, mode2);
//			std::cout << (int) mode << " ";
			btMatrix[pos/4] |= mode << (pos % 4) * 2;

			pos++;
		}
//		std::cout << std::endl;
		// swap rows
		scores * tmpPtr = prev_sM_G_D_vec;
		prev_sM_G_D_vec = curr_sM_G_D_vec;
		curr_sM_G_D_vec = tmpPtr;
	}

	// 0x03 00000011
#define get_val(bt, i, j) ( bt[(i * query_length + j)/4] >> (((i * query_length + j) % 4) * 2)  & 0x03 )
//    std::cout << std::endl;
//    std::cout << std::endl;
//  PRINT BT matrix
//    for (int i = 0; LIKELY(i < target_length); i++) {
//        std::cout << i << ": ";
//        for (int j = 0; LIKELY(j < query_length); j++) {
//            std::cout << get_val(btMatrix, i, j) << " ";
//        }
//        std::cout << std::endl;
//    }

	// backtrace
	int i=target_length - 1;
	int j=query_length - 1;
	int step = 0;
	int state = get_val(btMatrix, i, j);
	const int maxLength = std::max(query_length, target_length);
	result->seq = new uint32_t[ maxLength * 2];

	while (state!=-1)     // while (state!=STOP)  because STOP=0
	{
		//std::cout << step<< " " << i << " " << j << " " << state << std::endl;
		switch (state) {
			case M: // current state is MM, previous state is bMM[i][j]
				//	matched_cols++;
				result->seq[step] = to_cigar_int(1, 'M');

				if(i <= 0 || j <= 0){
					state = -1;
				}else{
					i--;
					j--;
					state =  get_val(btMatrix, i, j);
				}
				break;
			case E: // current state is GD
				result->seq[step] = to_cigar_int(1, 'I');

				if (j <= 0){
					state = -1;
				}else{
					j--;
					state =  get_val(btMatrix, i, j);
				}
				break;
			case F:
				result->seq[step] = to_cigar_int(1, 'D');

				if (i <= 0){
					state = -1;
				}else{
					i--;
					state = get_val(btMatrix, i, j);
				}
				break;
			default:
				Debug(Debug::ERROR) << "Wrong BT state " << state << " at i=" << i << " j="<< j << "\n";
				EXIT(EXIT_FAILURE);
		}
		step++;
	}
	std::reverse(result->seq, result->seq + step);
	result->length = step;
	return result;
#undef get_val
}

uint32_t SmithWaterman::to_cigar_int (uint32_t length, char op_letter)
{
	uint32_t res;
	uint8_t op_code;

	switch (op_letter) {
		case 'M': /* alignment match (can be a sequence match or mismatch */
		default:
			op_code = 0;
			break;
		case 'I': /* insertion to the reference */
			op_code = 1;
			break;
		case 'D': /* deletion from the reference */
			op_code = 2;
			break;
		case 'N': /* skipped region from the reference */
			op_code = 3;
			break;
		case 'S': /* soft clipping (clipped sequences present in SEQ) */
			op_code = 4;
			break;
		case 'H': /* hard clipping (clipped sequences NOT present in SEQ) */
			op_code = 5;
			break;
		case 'P': /* padding (silent deletion from padded reference) */
			op_code = 6;
			break;
		case '=': /* sequence match */
			op_code = 7;
			break;
		case 'X': /* sequence mismatch */
			op_code = 8;
			break;
	}

	res = (length << 4) | op_code;
	return res;
}

void SmithWaterman::printVector(__m128i v){
	for (int i = 0; i < 8; i++)
		printf("%d ", ((short) (sse2_extract_epi16(v, i)) + 32768));
	std::cout << "\n";
}

void SmithWaterman::printVectorUS(__m128i v){
	for (int i = 0; i < 8; i++)
		printf("%d ", (unsigned short) sse2_extract_epi16(v, i));
	std::cout << "\n";
}

unsigned short SmithWaterman::sse2_extract_epi16(__m128i v, int pos) {
	switch(pos){
		case 0: return _mm_extract_epi16(v, 0);
		case 1: return _mm_extract_epi16(v, 1);
		case 2: return _mm_extract_epi16(v, 2);
		case 3: return _mm_extract_epi16(v, 3);
		case 4: return _mm_extract_epi16(v, 4);
		case 5: return _mm_extract_epi16(v, 5);
		case 6: return _mm_extract_epi16(v, 6);
		case 7: return _mm_extract_epi16(v, 7);
	}
	std::cerr << "Fatal error in QueryScore: position in the vector is not in the legal range (pos = " << pos << ")\n";
	EXIT(1);
	// never executed
	return 0;
}
