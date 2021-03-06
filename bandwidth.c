/*-----------------------------------------------------------------------*/
/* Program: STREAM                                                       */
/* Revision: $Id: stream.c,v 5.10 2013/01/17 16:01:06 mccalpin Exp mccalpin $ */
/* Original code developed by John D. McCalpin                           */
/* Programmers: John D. McCalpin                                         */
/*              Joe R. Zagar                                             */
/*                                                                       */
/* This program measures memory transfer rates in MB/s for simple        */
/* computational kernels coded in C.                                     */
/*-----------------------------------------------------------------------*/
/* Copyright 1991-2013: John D. McCalpin                                 */
/*-----------------------------------------------------------------------*/
/* License:                                                              */
/*  1. You are free to use this program and/or to redistribute           */
/*     this program.                                                     */
/*  2. You are free to modify this program for your own use,             */
/*     including commercial use, subject to the publication              */
/*     restrictions in item 3.                                           */
/*  3. You are free to publish results obtained from running this        */
/*     program, or from works that you derive from this program,         */
/*     with the following limitations:                                   */
/*     3a. In order to be referred to as "STREAM benchmark results",     */
/*         published results must be in conformance to the STREAM        */
/*         Run Rules, (briefly reviewed below) published at              */
/*         http://www.cs.virginia.edu/stream/ref.html                    */
/*         and incorporated herein by reference.                         */
/*         As the copyright holder, John McCalpin retains the            */
/*         right to determine conformity with the Run Rules.             */
/*     3b. Results based on modified source code or on runs not in       */
/*         accordance with the STREAM Run Rules must be clearly          */
/*         labelled whenever they are published.  Examples of            */
/*         proper labelling include:                                     */
/*           "tuned STREAM benchmark results"                            */
/*           "based on a variant of the STREAM benchmark code"           */
/*         Other comparable, clear, and reasonable labelling is          */
/*         acceptable.                                                   */
/*     3c. Submission of results to the STREAM benchmark web site        */
/*         is encouraged, but not required.                              */
/*  4. Use of this program or creation of derived works based on this    */
/*     program constitutes acceptance of these licensing restrictions.   */
/*  5. Absolutely no warranty is expressed or implied.                   */
/*-----------------------------------------------------------------------*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

# include <stdio.h>
# include <unistd.h>
# include <math.h>
# include <float.h>
# include <limits.h>
# include <sys/time.h>
# include <sys/types.h>
# include <stdint.h>

#include <sys/stat.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>

#include <immintrin.h>

#ifdef MCDRAM
#include <hbwmalloc.h>
#endif

#ifndef STREAM_ARRAY_SIZE
#   define STREAM_ARRAY_SIZE	128000000
#endif

/*  2) STREAM runs each kernel "NTIMES" times and reports the *best* result
 *         for any iteration after the first, therefore the minimum value
 *         for NTIMES is 2.
 *      There are no rules on maximum allowable values for NTIMES, but
 *         values larger than the default are unlikely to noticeably
 *         increase the reported performance.
 *      NTIMES can also be set on the compile line without changing the source
 *         code using, for example, "-DNTIMES=7".
 */
#ifdef NTIMES
#if NTIMES<=1
#   define NTIMES	10
#endif
#endif
#ifndef NTIMES
#   define NTIMES	10
#endif

/*  Users are allowed to modify the "OFFSET" variable, which *may* change the
 *         relative alignment of the arrays (though compilers may change the 
 *         effective offset by making the arrays non-contiguous on some systems). 
 *      Use of non-zero values for OFFSET can be especially helpful if the
 *         STREAM_ARRAY_SIZE is set to a value close to a large power of 2.
 *      OFFSET can also be set on the compile line without changing the source
 *         code using, for example, "-DOFFSET=56".
 */
#ifndef OFFSET
#   define OFFSET	0
#endif

/*
 *	3) Compile the code with optimization.  Many compilers generate
 *       unreasonably bad code before the optimizer tightens things up.  
 *     If the results are unreasonably good, on the other hand, the
 *       optimizer might be too smart for me!
 *
 *     For a simple single-core version, try compiling with:
 *            cc -O stream.c -o stream
 *     This is known to work on many, many systems....
 *
 *     To use multiple cores, you need to tell the compiler to obey the OpenMP
 *       directives in the code.  This varies by compiler, but a common example is
 *            gcc -O -fopenmp stream.c -o stream_omp
 *       The environment variable OMP_NUM_THREADS allows runtime control of the 
 *         number of threads/cores used when the resulting "stream_omp" program
 *         is executed.
 *
 *     To run with single-precision variables and arithmetic, simply add
 *         -DSTREAM_TYPE=float
 *     to the compile line.
 *     Note that this changes the minimum array sizes required --- see (1) above.
 *
 *     The preprocessor directive "TUNED" does not do much -- it simply causes the 
 *       code to call separate functions to execute each kernel.  Trivial versions
 *       of these functions are provided, but they are *not* tuned -- they just 
 *       provide predefined interfaces to be replaced with tuned code.
 *
 *
 *	4) Optional: Mail the results to mccalpin@cs.virginia.edu
 *	   Be sure to include info that will help me understand:
 *		a) the computer hardware configuration (e.g., processor model, memory type)
 *		b) the compiler name/version and compilation flags
 *      c) any run-time information (such as OMP_NUM_THREADS)
 *		d) all of the output from the test case.
 *
 * Thanks!
 *
 *-----------------------------------------------------------------------*/

# define HLINE "-------------------------------------------------------------\n"

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

#ifndef STREAM_TYPE
#define STREAM_TYPE float
#endif

STREAM_TYPE	*a;//[STREAM_ARRAY_SIZE+OFFSET] __attribute__((aligned(64)));
STREAM_TYPE	*b;//[STREAM_ARRAY_SIZE+OFFSET] __attribute__((aligned(64)));
STREAM_TYPE	*c;//[STREAM_ARRAY_SIZE+OFFSET] __attribute__((aligned(64)));

static double	avgtime[4] = {0}, maxtime[4] = {0},
		mintime[4] = {FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX};

static char	*label[4] = {"Copy:      ", "Scale:     ",
	"Add:       ", "Triad:     "};

static double	bytes[4] = {
	2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
	2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
	3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
	3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE
};

extern double mysecond();
extern void checkSTREAMresults();
#ifdef TUNED
extern void tuned_STREAM_Copy();
extern void tuned_STREAM_Scale(STREAM_TYPE scalar);
extern void tuned_STREAM_Add();
extern void tuned_STREAM_Triad(STREAM_TYPE scalar);
#endif
#ifdef _OPENMP
extern int omp_get_num_threads();
#endif
int get_cpu_id (int tid)
{
//#ifdef SCATTER
//        int gid=tid%60;
//        int iid=(int)(floor(1.0*tid/60));
        //printf ("%d\n", (1+gid*4+iid)%240);
//        return (1+gid*4+iid)%240;
//#else
        return tid;
//#endif
}
typedef struct 
{
	STREAM_TYPE * a;	
	STREAM_TYPE * b;	
	STREAM_TYPE * c;
	uint32_t size;
	double time;	
	pthread_barrier_t *barrier;
}argc_t;
void *copy (void *parm)
{
	uint32_t i=0,j=0;
	argc_t *arg=(argc_t*)parm;
	STREAM_TYPE *a=arg->a;
	STREAM_TYPE *b=arg->b;
	double time;
	//printf ("%d\n", arg->size);
	pthread_barrier_wait(arg->barrier);
	time=mysecond();
	for (i=0;i<arg->size;i+=16)
	{
		__m512 key = _mm512_load_ps(&a[i]);
		_mm512_stream_ps (&b[i], key);		
	}
	pthread_barrier_wait(arg->barrier);
	time=mysecond()-time;
	arg->time=time;
	pthread_exit(NULL);
}


int main(int argc, char **argv)
{
	int			quantum, checktick();
	int			BytesPerWord;
	int			k;
	ssize_t		j;
	STREAM_TYPE		scalar;
	double		t, times[4][NTIMES];
	
	#ifdef MCDRAM
	if (hbw_check_available())//returns zero if hbw_malloc is availiable.
	{
		posix_memalign(&a, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
		posix_memalign(&b, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
		posix_memalign(&c, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
	}
	else
	{
		hbw_posix_memalign(&a, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
		hbw_posix_memalign(&b, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
		hbw_posix_memalign(&c, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
	}
	#else
		posix_memalign((void**)&a, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
		posix_memalign((void**)&b, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
		posix_memalign((void**)&c, 64, sizeof(STREAM_TYPE)*STREAM_ARRAY_SIZE+OFFSET);
	#endif

	/* --- SETUP --- determine precision and check timing --- */

	//printf(HLINE);
	//printf("STREAM version $Revision: 5.10 $\n");
	//printf(HLINE);
	BytesPerWord = sizeof(STREAM_TYPE);
	//printf("This system uses %d bytes per array element.\n",
	//BytesPerWord);

	//printf(HLINE);
	for (j=0; j<STREAM_ARRAY_SIZE; j++) {
		a[j] = 1.0;
		b[j] = 2.0;
		//c[j] = 0.0;
	}

/*	--- MAIN LOOP --- repeat test cases NTIMES times --- */

scalar = 3.0;
float tc=0;
int tt, threads;

threads=atoll(argv[1]);
//prepare for the pthread
pthread_t tid[288];
pthread_attr_t attr[288];
cpu_set_t set;
argc_t info[288];


for (k=0; k<NTIMES; k++)
{
	
#ifdef MP
	times[0][k] = mysecond();
#ifdef TUNED
	tuned_STREAM_Copy();
#else

	for (j=0; j<STREAM_ARRAY_SIZE; j+=1)
	{
		b[j]=a[j];
	}
	//_mm512_storenrngo_ps(&c[0], temp);
#endif
	times[0][k]=mysecond()-times[0][k];
#else
	times[0][k]=0;
	//times[0][k] = mysecond();
	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, threads);
	for (tt=0;tt<threads;++tt)
	{
		uint32_t size=(STREAM_ARRAY_SIZE/threads) & ~15;
		pthread_attr_init(&attr[tt]);
		CPU_ZERO(&set);
		CPU_SET(get_cpu_id(tt), &set);
		pthread_attr_setaffinity_np(&attr[tt], sizeof(cpu_set_t), &set);
		info[tt].a=&a[size*tt];
		info[tt].b=&b[size*tt];
		info[tt].c=&c[size*tt];
		info[tt].barrier=&barrier;
		info[tt].size=size;
	}
	//times[0][k] = mysecond();
	for (tt=0;tt<threads;++tt)
	{	
		pthread_create(&tid[tt], &attr[tt], copy, (void*) &info[tt]);		
	}	
	for (tt = 0 ; tt != threads ; ++tt)
	{
		pthread_join(tid[tt], NULL);
		times[0][k]+=info[tt].time;
	}
	times[0][k] = times[0][k]/threads;
#endif


	}

/*	--- SUMMARY --- */

for (k=1; k<NTIMES; k++) /* note -- skip first iteration */
{
	for (j=0; j<1; j++)
	{
		avgtime[j] = avgtime[j] + times[j][k];
		mintime[j] = MIN(mintime[j], times[j][k]);
		maxtime[j] = MAX(maxtime[j], times[j][k]);
	}
}

//printf("Function    Best Rate MB/s  Avg time     Min time     Max time\n");

for (j=0; j<1; j++) {
	avgtime[j] = avgtime[j]/(double)(NTIMES-1);

	printf("Threads:\t%d\tRead and write bandwidth (MB/s):\t%12.1f\n", threads,1.0E-06 * bytes[j]/mintime[j]);
}
return 0;
}

# define	M	20

	int
checktick()
{
	int		i, minDelta, Delta;
	double	t1, t2, timesfound[M];

	for (i = 0; i < M; i++) {
		t1 = mysecond();
		while( ((t2=mysecond()) - t1) < 1.0E-6 )
			;
		timesfound[i] = t1 = t2;
	}

	/*
	 * Determine the minimum difference between these M values.
	 * This result will be our estimate (in microseconds) for the
	 * clock granularity.
	 */

	minDelta = 1000000;
	for (i = 1; i < M; i++) {
		Delta = (int)( 1.0E6 * (timesfound[i]-timesfound[i-1]));
		minDelta = MIN(minDelta, MAX(Delta,0));
	}

	return(minDelta);
}



/* A gettimeofday routine to give access to the wall
   clock timer on most UNIX-like systems.  */

#include <sys/time.h>

double mysecond()
{
	struct timeval tp;
	struct timezone tzp;
	int i;

	i = gettimeofday(&tp,&tzp);
	return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

#ifndef abs
#define abs(a) ((a) >= 0 ? (a) : -(a))
#endif
void checkSTREAMresults ()
{
	STREAM_TYPE aj,bj,cj,scalar;
	STREAM_TYPE aSumErr,bSumErr,cSumErr;
	STREAM_TYPE aAvgErr,bAvgErr,cAvgErr;
	double epsilon;
	ssize_t	j;
	int	k,ierr,err;

	/* reproduce initialization */
	aj = 1.0;
	bj = 2.0;
	cj = 0.0;
	/* a[] is modified during timing check */
	aj = 2.0E0 * aj;
	/* now execute timing loop */
	scalar = 3.0;
	for (k=0; k<NTIMES; k++)
	{
		cj = aj;
		bj = scalar*cj;
		cj = aj+bj;
		aj = bj+scalar*cj;
	}

	/* accumulate deltas between observed and expected results */
	aSumErr = 0.0;
	bSumErr = 0.0;
	cSumErr = 0.0;
	for (j=0; j<STREAM_ARRAY_SIZE; j++) {
		aSumErr += abs(a[j] - aj);
		bSumErr += abs(b[j] - bj);
		cSumErr += abs(c[j] - cj);
		// if (j == 417) //printf("Index 417: c[j]: %f, cj: %f\n",c[j],cj);	// MCCALPIN
	}
	aAvgErr = aSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;
	bAvgErr = bSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;
	cAvgErr = cSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;

	if (sizeof(STREAM_TYPE) == 4) {
		epsilon = 1.e-6;
	}
	else if (sizeof(STREAM_TYPE) == 8) {
		epsilon = 1.e-13;
	}
	else {
		//printf("WEIRD: sizeof(STREAM_TYPE) = %lu\n",sizeof(STREAM_TYPE));
		epsilon = 1.e-6;
	}

	err = 0;
	if (abs(aAvgErr/aj) > epsilon) {
		err++;
		//printf ("Failed Validation on array a[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		//printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",aj,aAvgErr,abs(aAvgErr)/aj);
		ierr = 0;
		for (j=0; j<STREAM_ARRAY_SIZE; j++) {
			if (abs(a[j]/aj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					//printf("         array a: index: %ld, expected: %e, observed: %e, relative error: %e\n",
					j,aj,a[j],abs((aj-a[j])/aAvgErr));
				}
#endif
			}
		}
		//printf("     For array a[], %d errors were found.\n",ierr);
	}
	if (abs(bAvgErr/bj) > epsilon) {
		err++;
		//printf ("Failed Validation on array b[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		//printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",bj,bAvgErr,abs(bAvgErr)/bj);
		//printf ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
		ierr = 0;
		for (j=0; j<STREAM_ARRAY_SIZE; j++) {
			if (abs(b[j]/bj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					//printf("         array b: index: %ld, expected: %e, observed: %e, relative error: %e\n",
					j,bj,b[j],abs((bj-b[j])/bAvgErr));
				}
#endif
			}
		}
		//printf("     For array b[], %d errors were found.\n",ierr);
	}
	if (abs(cAvgErr/cj) > epsilon) {
		err++;
		//printf ("Failed Validation on array c[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		//printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",cj,cAvgErr,abs(cAvgErr)/cj);
		//printf ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
		ierr = 0;
		for (j=0; j<STREAM_ARRAY_SIZE; j++) {
			if (abs(c[j]/cj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					//printf("         array c: index: %ld, expected: %e, observed: %e, relative error: %e\n",
					j,cj,c[j],abs((cj-c[j])/cAvgErr));
				}
#endif
			}
		}
		//printf("     For array c[], %d errors were found.\n",ierr);
	}
	if (err == 0) {
		//printf ("Solution Validates: avg error less than %e on all three arrays\n",epsilon);
	}
#ifdef VERBOSE
	//printf ("Results Validation Verbose Results: \n");
	//printf ("    Expected a(1), b(1), c(1): %f %f %f \n",aj,bj,cj);
	//printf ("    Observed a(1), b(1), c(1): %f %f %f \n",a[1],b[1],c[1]);
	//printf ("    Rel Errors on a, b, c:     %e %e %e \n",abs(aAvgErr/aj),abs(bAvgErr/bj),abs(cAvgErr/cj));
#endif
}

#ifdef TUNED
/* stubs for "tuned" versions of the kernels */
void tuned_STREAM_Copy()
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
		c[j] = a[j];
}

void tuned_STREAM_Scale(STREAM_TYPE scalar)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
		b[j] = scalar*c[j];
}

void tuned_STREAM_Add()
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
		c[j] = a[j]+b[j];
}

void tuned_STREAM_Triad(STREAM_TYPE scalar)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
		a[j] = b[j]+scalar*c[j];
}
/* end of stubs for the "tuned" versions of the kernels */
#endif
