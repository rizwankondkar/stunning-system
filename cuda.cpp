﻿#include <cstdio>
#include <memory.h>
#include <cstring>
#include <map>
using namespace std;
#ifndef _WIN32
#include <unistd.h>
#endif
// include thrust
#ifndef __cplusplus
#include <thrust/version.h>
#include <thrust/remove.h>
#include <thrust/device_vector.h>
#include <thrust/iterator/constant_iterator.h>
#else
#include <ctype.h>
#endif

#include "nvml.h"
#include "miner.h"

#include "cuda_runtime.h"

cudaDeviceProp device_props[MAX_GPUS];
cudaStream_t gpustream[MAX_GPUS] = { 0 };
extern uint16_t opt_api_listen;

// CUDA Devices on the System
int cuda_num_devices()
{
	int version;
	cudaError_t err = cudaDriverGetVersion(&version);
	if (err != cudaSuccess)
	{
		applog(LOG_ERR, "Unable to query CUDA driver version! Is an nVidia driver installed?");
		exit(1);
	}

	if (version < CUDART_VERSION)
	{
		applog(LOG_ERR, "Driver does not support CUDA %d.%d API! Update your nVidia driver!", CUDART_VERSION / 1000, (CUDART_VERSION % 1000) / 10);
		exit(1);
	}

	int GPU_N;
	err = cudaGetDeviceCount(&GPU_N);
	if (err != cudaSuccess)
	{
		if(err!=cudaErrorNoDevice)
			applog(LOG_ERR, "No CUDA device found!");
		else
			applog(LOG_ERR, "Unable to query number of CUDA devices!");
		exit(1);
	}
	return GPU_N;
}

int cuda_version()
{
	return (int)CUDART_VERSION;
}

void cuda_devicenames()
{
	cudaError_t err;
	int GPU_N;
	err = cudaGetDeviceCount(&GPU_N);
	if(err != cudaSuccess)
	{
		applog(LOG_ERR, "Unable to query number of CUDA devices! Is an nVidia driver installed?");
		exit(1);
	}

	if(opt_n_threads)
		GPU_N = min(MAX_GPUS, opt_n_threads);
	for(int i = 0; i < GPU_N; i++)
	{
		char vendorname[32] = {0};
		int dev_id = device_map[i];
		cudaDeviceProp props;
		cudaGetDeviceProperties(&props, dev_id);

		device_sm[dev_id] = (props.major * 100 + props.minor * 10);

		if(device_name[dev_id])
		{
			free(device_name[dev_id]);
			device_name[dev_id] = NULL;
		}
#ifdef USE_WRAPNVML
		if(gpu_vendor((uint8_t)props.pciBusID, vendorname) > 0 && strlen(vendorname))
		{
			device_name[dev_id] = (char*)calloc(1, strlen(vendorname) + strlen(props.name) + 2);
			if(device_name[dev_id] == NULL)
			{
				applog(LOG_ERR, "Out of memory!");
				proper_exit(1);
			}
			if(!strncmp(props.name, "GeForce ", 8))
				sprintf(device_name[dev_id], "%s %s", vendorname, &props.name[8]);
			else
				sprintf(device_name[dev_id], "%s %s", vendorname, props.name);
		}
		else
#endif
			device_name[dev_id] = strdup(props.name);
	}
}


void cuda_print_devices()
{
	int ngpus = cuda_num_devices();
	for (int n=0; n < ngpus; n++) {
		int m = device_map[n];
		cudaDeviceProp props;
		cudaGetDeviceProperties(&props, m);
		if (!opt_n_threads || n < opt_n_threads)
			fprintf(stderr, "GPU #%d: SM %d.%d %s\n", m, props.major, props.minor, props.name);
	}
}

// Can't be called directly in cpu-miner.c
void cuda_devicereset()
{
	for (int i = 0; i < active_gpus; i++)
	{
		cudaSetDevice(device_map[i]);
		cudaDeviceSynchronize();
		cudaDeviceReset();
	}
}

static bool substringsearch(const char *haystack, const char *needle, int &match)
{
	int hlen = (int) strlen(haystack);
	int nlen = (int) strlen(needle);
	for (int i=0; i < hlen; ++i)
	{
		if (haystack[i] == ' ') continue;
		int j=0, x = 0;
		while(j < nlen)
		{
			if (haystack[i+x] == ' ') {++x; continue;}
			if (needle[j] == ' ') {++j; continue;}
			if (needle[j] == '#') return ++match == needle[j+1]-'0';
			if (tolower(haystack[i+x]) != tolower(needle[j])) break;
			++j; ++x;
		}
		if (j == nlen) return true;
	}
	return false;
}

// CUDA Gerät nach Namen finden (gibt Geräte-Index zurück oder -1)
int cuda_finddevice(char *name)
{
	int num = cuda_num_devices();
	int match = 0;
	for (int i=0; i < num; ++i)
	{
		cudaDeviceProp props;
		if (cudaGetDeviceProperties(&props, i) == cudaSuccess)
			if (substringsearch(props.name, name, match)) return i;
	}
	return -1;
}

uint32_t device_intensity(int thr_id, const char *func, uint32_t defcount)
{
	uint32_t throughput = gpus_intensity[thr_id] ? gpus_intensity[thr_id] : defcount;
	if(opt_api_listen!=0) api_set_throughput(thr_id, throughput);
	return throughput;
}

// Zeitsynchronisations-Routine von cudaminer mit CPU sleep
typedef struct { double value[8]; } tsumarray;

cudaError_t MyStreamSynchronize(cudaStream_t stream, int situation, int thr_id)
{
	cudaError_t result = cudaSuccess;
	if (situation >= 0)
	{
		static std::map<int, tsumarray> tsum;
		double tsync = 0.0;
		double tsleep = 0.95;

		double a = 0.95, b = 0.05;
		if (tsum.find(situation) == tsum.end()) { a = 0.5; b = 0.5; } // faster initial convergence
		tsleep = 0.95*tsum[situation].value[thr_id];
		if (cudaStreamQuery(stream) == cudaErrorNotReady)
		{
			usleep((useconds_t)(1e6*tsleep));
			struct timeval tv_start, tv_end;
			gettimeofday(&tv_start, NULL);
			result = cudaStreamSynchronize(stream);
			gettimeofday(&tv_end, NULL);
			tsync = 1e-6 * (tv_end.tv_usec - tv_start.tv_usec) + (tv_end.tv_sec - tv_start.tv_sec);
		}
		if (tsync >= 0) tsum[situation].value[thr_id] = a * tsum[situation].value[thr_id] + b * (tsleep + tsync);
	}
	else
		result = cudaStreamSynchronize(stream);
	return result;
}


int cuda_gpu_clocks(struct cgpu_info *gpu)
{
	cudaDeviceProp props;
	if (cudaGetDeviceProperties(&props, gpu->gpu_id) == cudaSuccess) {
		gpu->gpu_clock = props.clockRate;
		gpu->gpu_memclock = props.memoryClockRate;
		gpu->gpu_mem = props.totalGlobalMem;
		return 0;
	}
	return -1;
}

void cudaReportHardwareFailure(int thr_id, cudaError_t err, const char* func)
{
	struct cgpu_info *gpu = &thr_info[thr_id].gpu;
	gpu->hw_errors++;
	applog(LOG_ERR, "GPU #%d: %s %s", device_map[thr_id], func, cudaGetErrorString(err));
	sleep(1);
}
