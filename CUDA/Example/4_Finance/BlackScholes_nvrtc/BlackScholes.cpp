/*
 * Copyright 1993-2015 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

/*
 * This sample evaluates fair call and put prices for a
 * given set of European options by Black-Scholes formula.
 * See supplied whitepaper for more explanations.
 */

#include <cuda_runtime.h>
#include <nvrtc_helper.h>
#include <cudaProfiler.h>

#include <helper_functions.h>   // helper functions for string parsing


////////////////////////////////////////////////////////////////////////////////
// Process an array of optN options on CPU
////////////////////////////////////////////////////////////////////////////////

extern "C" void BlackScholesCPU(
    float *h_CallResult,
    float *h_PutResult,
    float *h_StockPrice,
    float *h_OptionStrike,
    float *h_OptionYears,
    float Riskfree,
    float Volatility,
    int optN
);


////////////////////////////////////////////////////////////////////////////////
// Process an array of OptN options on GPU
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

// Helper function, returning uniformly distributed
// random float in [low, high] range
////////////////////////////////////////////////////////////////////////////////

float RandFloat(float low, float high)
{
    float t = (float)rand() / (float)RAND_MAX;
    return (1.0f - t) * low + t * high;
}

////////////////////////////////////////////////////////////////////////////////
// Data configuration
////////////////////////////////////////////////////////////////////////////////

const int   OPT_N = 4000000;
const int   NUM_ITERATIONS = 512;
const int   OPT_SZ = OPT_N * sizeof(float);
const float RISKFREE = 0.02f;
const float VOLATILITY = 0.30f;

#define DIV_UP(a, b) ( ((a) + (b) - 1) / (b) )

////////////////////////////////////////////////////////////////////////////////
// Main program
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char **argv)
{
    // Start logs
    printf("[%s] - Starting...\n", argv[0]);

    //'h_' prefix - CPU (host) memory space
    float
    //Results calculated by CPU for reference
    *h_CallResultCPU,
    *h_PutResultCPU,
    //CPU copy of GPU results
    *h_CallResultGPU,
    *h_PutResultGPU,
    //CPU instance of input data
    *h_StockPrice,
    *h_OptionStrike,
    *h_OptionYears;

    //'d_' prefix - GPU (device) memory space
    CUdeviceptr
    //Results calculated by GPU
    d_CallResult,
    d_PutResult,

    //GPU instance of input data
    d_StockPrice,
    d_OptionStrike,
    d_OptionYears;

    double
    delta, ref, sum_delta, sum_ref, max_delta, L1norm, gpuTime;

    StopWatchInterface *hTimer = NULL;
    int i;

    sdkCreateTimer(&hTimer);

    printf("Initializing data...\n");
    printf("...allocating CPU memory for options.\n");

    h_CallResultCPU = (float *)malloc(OPT_SZ);
    h_PutResultCPU  = (float *)malloc(OPT_SZ);
    h_CallResultGPU = (float *)malloc(OPT_SZ);
    h_PutResultGPU  = (float *)malloc(OPT_SZ);
    h_StockPrice    = (float *)malloc(OPT_SZ);
    h_OptionStrike  = (float *)malloc(OPT_SZ);
    h_OptionYears   = (float *)malloc(OPT_SZ);


    char *ptx, *kernel_file;
    size_t ptxSize;
    kernel_file = sdkFindFilePath("BlackScholes_kernel.cuh", argv[0]);

    // Set a Compiler Option to have maximum register to be used by each thread.
    char *compile_options[1];
    compile_options[0] = (char *) malloc(sizeof(char)*(strlen("--maxrregcount=16")));
    strcpy((char *)compile_options[0],"--maxrregcount=16");

    // Compile the kernel BlackScholes_kernel.
    compileFileToPTX(kernel_file, 1, (const char **)compile_options, &ptx, &ptxSize);
    CUmodule module = loadPTX(ptx, argc, argv);

    CUfunction kernel_addr;
    checkCudaErrors(cuModuleGetFunction(&kernel_addr, module, "BlackScholesGPU"));

    printf("...allocating GPU memory for options.\n");
    checkCudaErrors(cuMemAlloc(&d_CallResult, OPT_SZ));
    checkCudaErrors(cuMemAlloc(&d_PutResult, OPT_SZ));
    checkCudaErrors(cuMemAlloc(&d_StockPrice, OPT_SZ));
    checkCudaErrors(cuMemAlloc(&d_OptionStrike,OPT_SZ));
    checkCudaErrors(cuMemAlloc(&d_OptionYears, OPT_SZ));

    printf("...generating input data in CPU mem.\n");
    srand(5347);

    //Generate options set
    for (i = 0; i < OPT_N; i++)
    {
        h_CallResultCPU[i] = 0.0f;
        h_PutResultCPU[i]  = -1.0f;
        h_StockPrice[i]    = RandFloat(5.0f, 30.0f);
        h_OptionStrike[i]  = RandFloat(1.0f, 100.0f);
        h_OptionYears[i]   = RandFloat(0.25f, 10.0f);
    }

    printf("...copying input data to GPU mem.\n");
    //Copy options data to GPU memory for further processing
    checkCudaErrors(cuMemcpyHtoD(d_StockPrice, h_StockPrice, OPT_SZ));
    checkCudaErrors(cuMemcpyHtoD(d_OptionStrike, h_OptionStrike, OPT_SZ));
    checkCudaErrors(cuMemcpyHtoD(d_OptionYears, h_OptionYears, OPT_SZ));

    printf("Data init done.\n\n");
    printf("Executing Black-Scholes GPU kernel (%i iterations)...\n", NUM_ITERATIONS);

    sdkResetTimer(&hTimer);
    sdkStartTimer(&hTimer);

    dim3 cudaBlockSize( 128, 1, 1);
    dim3 cudaGridSize(DIV_UP(OPT_N/2, 128),1,1);

    float risk = RISKFREE;
    float volatility = VOLATILITY;
    int optval = OPT_N;

    void *arr[] = { (void *)&d_CallResult, (void *)&d_PutResult, (void *)&d_StockPrice,
        (void *)&d_OptionStrike, (void *)&d_OptionYears, (void *)&risk, (void *)&volatility, (void *)&optval };

    for (i = 0; i < NUM_ITERATIONS; i++)
    {

        checkCudaErrors(cuLaunchKernel(kernel_addr,
                                            cudaGridSize.x, cudaGridSize.y, cudaGridSize.z, /* grid dim */
                                            cudaBlockSize.x, cudaBlockSize.y, cudaBlockSize.z, /* block dim */
                                            0,0, /* shared mem, stream */
                                            &arr[0], /* arguments */
                                            0));

    }

    checkCudaErrors(cuCtxSynchronize());

    sdkStopTimer(&hTimer);
    gpuTime = sdkGetTimerValue(&hTimer) / NUM_ITERATIONS;

    //Both call and put is calculated
    printf("Options count             : %i     \n", 2 * OPT_N);
    printf("BlackScholesGPU() time    : %f msec\n", gpuTime);
    printf("Effective memory bandwidth: %f GB/s\n", ((double)(5 * OPT_N * sizeof(float)) * 1E-9) / (gpuTime * 1E-3));
    printf("Gigaoptions per second    : %f     \n\n", ((double)(2 * OPT_N) * 1E-9) / (gpuTime * 1E-3));
    printf("BlackScholes, Throughput = %.4f GOptions/s, Time = %.5f s, Size = %u options, NumDevsUsed = %u, Workgroup = %u\n",
           (((double)(2.0 * OPT_N) * 1.0E-9) / (gpuTime * 1.0E-3)), gpuTime*1e-3, (2 * OPT_N), 1, 128);

    printf("\nReading back GPU results...\n");

    //Read back GPU results to compare them to CPU results
    checkCudaErrors(cuMemcpyDtoH(h_CallResultGPU, d_CallResult, OPT_SZ));
    checkCudaErrors(cuMemcpyDtoH(h_PutResultGPU, d_PutResult, OPT_SZ));

    printf("Checking the results...\n");
    printf("...running CPU calculations.\n\n");

    //Calculate options values on CPU
    BlackScholesCPU(
        h_CallResultCPU,
        h_PutResultCPU,
        h_StockPrice,
        h_OptionStrike,
        h_OptionYears,
        RISKFREE,
        VOLATILITY,
        OPT_N
    );

    printf("Comparing the results...\n");
    //Calculate max absolute difference and L1 distance
    //between CPU and GPU results
    sum_delta = 0;
    sum_ref   = 0;
    max_delta = 0;

    for (i = 0; i < OPT_N; i++)
    {
        ref   = h_CallResultCPU[i];
        delta = fabs(h_CallResultCPU[i] - h_CallResultGPU[i]);

        if (delta > max_delta)
        {
            max_delta = delta;
        }

        sum_delta += delta;
        sum_ref   += fabs(ref);
    }

    L1norm = sum_delta / sum_ref;
    printf("L1 norm: %E\n", L1norm);
    printf("Max absolute error: %E\n\n", max_delta);

    printf("Shutting down...\n");
    printf("...releasing GPU memory.\n");

    checkCudaErrors(cuMemFree(d_OptionYears));
    checkCudaErrors(cuMemFree(d_OptionStrike));
    checkCudaErrors(cuMemFree(d_StockPrice));
    checkCudaErrors(cuMemFree(d_PutResult));
    checkCudaErrors(cuMemFree(d_CallResult));

    printf("...releasing CPU memory.\n");

    free(h_OptionYears);
    free(h_OptionStrike);
    free(h_StockPrice);
    free(h_PutResultGPU);
    free(h_CallResultGPU);
    free(h_PutResultCPU);
    free(h_CallResultCPU);

    sdkDeleteTimer(&hTimer);
    printf("Shutdown done.\n");

    printf("\n[%s] - Test Summary\n", argv[0]);

    cuProfilerStop();

    if (L1norm > 1e-6)
    {
        printf("Test failed!\n");
        exit(EXIT_FAILURE);
    }

    printf("Test passed\n");
    exit(EXIT_SUCCESS);
}
