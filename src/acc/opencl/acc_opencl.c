/*------------------------------------------------------------------------------------------------*/
/* Copyright (C) by the DBCSR developers group - All rights reserved                              */
/* This file is part of the DBCSR library.                                                        */
/*                                                                                                */
/* For information on the license, see the LICENSE file.                                          */
/* For further information please visit https://dbcsr.cp2k.org                                    */
/* SPDX-License-Identifier: GPL-2.0+                                                              */
/*------------------------------------------------------------------------------------------------*/
#if defined(__OPENCL)
#  include "acc_opencl.h"
#  include <string.h>
#  include <limits.h>
#  include <ctype.h>
#  if defined(_WIN32)
#    include <windows.h>
#    include <process.h>
#  else
#    include <unistd.h>
#    include <errno.h>
#    include <glob.h>
#  endif
#  if defined(__DBCSR_ACC)
#    include "../acc_libsmm.h"
#  endif

#  include <sys/stat.h>
#  if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#    define S_ISDIR(A) ((S_IFMT & (A)) == S_IFDIR)
#  endif

#  if !defined(ACC_OPENCL_TEMPDIR) && 1
#    define ACC_OPENCL_TEMPDIR "/tmp"
#  endif
#  if !defined(ACC_OPENCL_CACHEDIR) && 0
#    define ACC_OPENCL_CACHEDIR ".cl_cache"
#  endif
#  if !defined(ACC_OPENCL_CPPBIN) && 1
#    define ACC_OPENCL_CPPBIN "/usr/bin/cpp"
#  endif
#  if !defined(ACC_OPENCL_SEDBIN) && 1
#    define ACC_OPENCL_SEDBIN "/usr/bin/sed"
#  endif
#  if !defined(ACC_OPENCL_NCCS) && 1
#    define ACC_OPENCL_NCCS 4
#  endif
#  if !defined(ACC_OPENCL_IENV) && 1
#    define ACC_OPENCL_IENV
#  endif


#  if defined(__cplusplus)
extern "C" {
#  endif

/* global configuration discovered during initialization */
c_dbcsr_acc_opencl_config_t c_dbcsr_acc_opencl_config;


void c_dbcsr_acc_opencl_notify(const char /*errinfo*/[], const void* /*private_info*/, size_t /*cb*/, void* /*user_data*/);
void c_dbcsr_acc_opencl_notify(const char errinfo[], const void* private_info, size_t cb, void* user_data) {
  LIBXSMM_UNUSED(private_info);
  LIBXSMM_UNUSED(cb);
  LIBXSMM_UNUSED(user_data);
  fprintf(stderr, "ERROR ACC/OpenCL: %s\n", errinfo);
}


cl_context c_dbcsr_acc_opencl_context(int* thread_id) {
  cl_context result;
  const int tid = ACC_OPENCL_OMP_TID();
  assert(0 <= tid && tid < c_dbcsr_acc_opencl_config.nthreads);
  assert(NULL != c_dbcsr_acc_opencl_config.device);
  result = c_dbcsr_acc_opencl_config.device[tid].context;
  if (NULL == result) { /* fallback */
    int i = 0; /* prefer master's context */
    for (; i < c_dbcsr_acc_opencl_config.nthreads; ++i) {
      if (tid != i) { /* adopt another context */
        result = c_dbcsr_acc_opencl_config.device[i].context;
        if (NULL != result && CL_SUCCESS == clRetainContext(result)) break;
        else result = NULL;
      }
    }
  }
  if (NULL != thread_id) *thread_id = tid;
  return result;
}


cl_context c_dbcsr_acc_opencl_device_context(cl_device_id device, const int* thread_id) {
  const int i0 = (NULL != thread_id ? *thread_id : /*main*/ 0);
  cl_context result = NULL;
  int i = 0;
  for (; i < c_dbcsr_acc_opencl_config.nthreads; ++i) {
    const int j = i + i0, tid = (j < c_dbcsr_acc_opencl_config.nthreads ? j : (j - c_dbcsr_acc_opencl_config.nthreads));
    result = c_dbcsr_acc_opencl_config.device[tid].context;
    if (NULL != result) {
      cl_device_id device_id = NULL;
      if (CL_SUCCESS == clGetContextInfo(result, CL_CONTEXT_DEVICES, sizeof(cl_device_id), &device_id, NULL) && device == device_id)
      {
        break;
      }
      else result = NULL;
    }
  }
  return result;
}


/**
 * Comparator used with qsort; stabilized by tail condition (a < b ? -1 : 1).
 * Brings GPUs with local memory in front, followed by (potentially) integrated GPUs,
 * and further orders by memory capacity.
 */
int c_dbcsr_acc_opencl_order_devices(const void* /*dev_a*/, const void* /*dev_b*/);
int c_dbcsr_acc_opencl_order_devices(const void* dev_a, const void* dev_b) {
  const cl_device_id* const a = (const cl_device_id*)dev_a;
  const cl_device_id* const b = (const cl_device_id*)dev_b;
  cl_device_type type_a = 0, type_b = 0;
  assert(NULL != a && NULL != b && a != b);
  ACC_OPENCL_EXPECT(EXIT_SUCCESS == clGetDeviceInfo(*a, CL_DEVICE_TYPE, sizeof(cl_device_type), &type_a, NULL));
  ACC_OPENCL_EXPECT(EXIT_SUCCESS == clGetDeviceInfo(*b, CL_DEVICE_TYPE, sizeof(cl_device_type), &type_b, NULL));
  if (CL_DEVICE_TYPE_DEFAULT & type_a) {
    return -1;
  }
  else if (CL_DEVICE_TYPE_DEFAULT & type_b) {
    return 1;
  }
  else {
    if (CL_DEVICE_TYPE_GPU & type_a) {
      if (CL_DEVICE_TYPE_GPU & type_b) {
        int unified_a, unified_b;
        size_t size_a, size_b;
        ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_info_devmem(*a, NULL, &size_a, NULL, &unified_a));
        ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_info_devmem(*b, NULL, &size_b, NULL, &unified_b));
        if ((0 == unified_a && 0 == unified_b) || (0 != unified_a && 0 != unified_b)) {
          return (size_a < size_b ? 1 : (size_a != size_b ? -1 : (a < b ? -1 : 1)));
        }
        /* discrete GPU goes in front */
        else if (0 == unified_b) return 1;
        else return -1;
      }
      else return -1;
    }
    else if (CL_DEVICE_TYPE_GPU & type_b) {
      return 1;
    }
    else {
      if (CL_DEVICE_TYPE_CPU & type_a) {
        if (CL_DEVICE_TYPE_CPU & type_b) {
          size_t size_a, size_b;
          ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_info_devmem(*a, NULL, &size_a, NULL, NULL));
          ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_info_devmem(*b, NULL, &size_b, NULL, NULL));
          return (size_a < size_b ? 1 : (size_a != size_b ? -1 : (a < b ? -1 : 1)));
        }
        else return -1;
      }
      else if (CL_DEVICE_TYPE_CPU & type_b) {
        return 1;
      }
      else {
        size_t size_a = 0, size_b = 0;
        ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_info_devmem(*a, NULL, &size_a, NULL, NULL));
        ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_info_devmem(*b, NULL, &size_b, NULL, NULL));
        return (size_a < size_b ? 1 : (size_a != size_b ? -1 : (a < b ? -1 : 1)));
      }
    }
  }
}


LIBXSMM_ATTRIBUTE_CTOR void c_dbcsr_acc_opencl_init(void) {
  /* attempt to  automatically initialize backend */
  ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_init());
}


LIBXSMM_ATTRIBUTE_DTOR void c_dbcsr_acc_opencl_finalize(void) {
  /* attempt to automatically finalize backend */
  ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_finalize());
}


int c_dbcsr_acc_init(void) {
#  if defined(_OPENMP)
  /* initialization/finalization is not meant to be thread-safe */
  int result = ((0 == omp_in_parallel() || /*main*/ 0 == omp_get_thread_num()) ? EXIT_SUCCESS : EXIT_FAILURE);
#  else
  int result = EXIT_SUCCESS;
#  endif
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  int routine_handle;
  static const char* const routine_name_ptr = LIBXSMM_FUNCNAME;
  static const int routine_name_len = (int)sizeof(LIBXSMM_FUNCNAME) - 1;
  c_dbcsr_timeset((const char**)&routine_name_ptr, &routine_name_len, &routine_handle);
#  endif
  if (EXIT_SUCCESS == result && 0 == c_dbcsr_acc_opencl_config.ndevices) { /* avoid to initialize multiple times */
    cl_platform_id platforms[ACC_OPENCL_DEVICES_MAXCOUNT] = {NULL};
    cl_device_id devices[ACC_OPENCL_DEVICES_MAXCOUNT];
    char buffer[ACC_OPENCL_BUFFERSIZE];
    const char *const env_devmatch = getenv("ACC_OPENCL_DEVMATCH"), *const env_devtype = getenv("ACC_OPENCL_DEVTYPE");
    const char *const env_priority = getenv("ACC_OPENCL_PRIORITY"), *const env_xhints = getenv("ACC_OPENCL_XHINTS");
    const char *const env_devcopy = getenv("ACC_OPENCL_DEVCOPY"), *const env_verbose = getenv("ACC_OPENCL_VERBOSE");
    const char *const env_device = getenv("ACC_OPENCL_DEVICE"), *const env_dump_acc = getenv("ACC_OPENCL_DUMP");
    const char *const env_async = getenv("ACC_OPENCL_ASYNC"), *const env_timer = getenv("ACC_OPENCL_TIMER");
    const char* const env_dump = (NULL != env_dump_acc ? env_dump_acc : getenv("IGC_ShaderDumpEnable"));
#  if defined(ACC_OPENCL_NCCS) && (0 < ACC_OPENCL_NCCS)
    const char *const env_zex = getenv("ZEX_NUMBER_OF_CCS"), *const env_nccs = getenv("ACC_OPENCL_NCCS");
    const char* const env_flt = getenv("ZE_FLAT_DEVICE_HIERARCHY");
    const int nccs = (NULL == env_nccs ? 0 : atoi(env_nccs));
#  endif
#  if defined(ACC_OPENCL_IENV)
    const char *const env_neo = getenv("NEOReadDebugKeys"), *const env_ienv = getenv("ACC_OPENCL_IENV");
    const int neo = (NULL == env_neo ? 1 : atoi(env_neo)), ienv = neo * (NULL == env_ienv ? 1 : atoi(env_ienv));
#  endif
    char* const env_devids = getenv("ACC_OPENCL_DEVIDS");
    int device_id = (NULL == env_device ? 0 : atoi(env_device));
    cl_uint nplatforms = 0, ndevices = 0, i;
    cl_device_type type = CL_DEVICE_TYPE_ALL;
#  if defined(_OPENMP)
    const int max_threads = omp_get_max_threads(), num_threads = omp_get_num_threads();
    c_dbcsr_acc_opencl_config.nthreads = (num_threads < max_threads ? max_threads : num_threads);
    c_dbcsr_acc_opencl_config.nstreams = (num_threads < max_threads ? (ACC_OPENCL_STREAMS_MAXCOUNT + max_threads)
                                                                    : (ACC_OPENCL_STREAMS_MAXCOUNT));
#  else
    c_dbcsr_acc_opencl_config.nthreads = 1;
    c_dbcsr_acc_opencl_config.nstreams = ACC_OPENCL_STREAMS_MAXCOUNT;
#  endif
    c_dbcsr_acc_opencl_config.verbosity = (NULL == env_verbose ? 0 : atoi(env_verbose));
    c_dbcsr_acc_opencl_config.priority = (NULL == env_priority ? /*default*/ 3 : atoi(env_priority));
    c_dbcsr_acc_opencl_config.devcopy = (NULL == env_devcopy ? /*default*/ 0 : atoi(env_devcopy));
    c_dbcsr_acc_opencl_config.xhints = (NULL == env_xhints ? /*default*/ 5 : atoi(env_xhints));
    c_dbcsr_acc_opencl_config.async = (NULL == env_async ? /*default*/ 3 : atoi(env_async));
    c_dbcsr_acc_opencl_config.dump = (NULL == env_dump ? /*default*/ 0 : atoi(env_dump));
    if (EXIT_SUCCESS != c_dbcsr_acc_opencl_device_uid(NULL /*device*/, env_devmatch, &c_dbcsr_acc_opencl_config.devmatch)) {
      c_dbcsr_acc_opencl_config.devmatch = 1;
    }
    libxsmm_init();
    if (NULL != env_timer && (c_dbcsr_acc_opencl_timer_host == atoi(env_timer) ||
                               (env_timer == LIBXSMM_STRISTR(env_timer, "host") && 4 == strlen(env_timer)) ||
                               (env_timer == LIBXSMM_STRISTR(env_timer, "cpu") && 3 == strlen(env_timer))))
    {
      c_dbcsr_acc_opencl_config.timer = c_dbcsr_acc_opencl_timer_host;
    }
#  if defined(ACC_OPENCL_NCCS) && (0 < ACC_OPENCL_NCCS)
    if ((NULL == env_zex && NULL == env_flt && 0 == (4 & c_dbcsr_acc_opencl_config.xhints)) ||
        (0 == LIBXSMM_PUTENV("ZE_FLAT_DEVICE_HIERARCHY=COMPOSITE") && 0 != nccs))
    {
      static char zex_number_of_ccs[ACC_OPENCL_DEVICES_MAXCOUNT * 8 + 32] = "ZEX_NUMBER_OF_CCS=";
      int j = strlen(zex_number_of_ccs);
      for (i = 0; i < ACC_OPENCL_DEVICES_MAXCOUNT; ++i) {
        const int n = LIBXSMM_SNPRINTF(
          zex_number_of_ccs + j, sizeof(zex_number_of_ccs) - j, 0 < i ? ",%u:%i" : "%u:%i", i, 0 >= nccs ? ACC_OPENCL_NCCS : nccs);
        if (0 < n) j += n;
        else {
          j = 0;
          break;
        }
      }
      if (0 < j) ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV(zex_number_of_ccs)); /* soft-error */
    }
#  endif
#  if defined(ACC_OPENCL_IENV)
    if (0 != ienv) {
      if (NULL == getenv("NEOReadDebugKeys")) ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV("NEOReadDebugKeys=1"));
      if (NULL == getenv("EnableRecoverablePageFaults")) {
        ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV("EnableRecoverablePageFaults=0"));
      }
    }
#  endif
#  if defined(ACC_OPENCL_CACHEDIR)
    {
      const char *const env_cache = getenv("ACC_OPENCL_CACHE"), *env_cachedir = getenv("NEO_CACHE_DIR");
      int cache = (NULL == env_cache ? 0 : atoi(env_cache));
      struct stat cachedir;
      if (0 == cache) {
        if (stat(ACC_OPENCL_CACHEDIR, &cachedir) == 0 && S_ISDIR(cachedir.st_mode)) cache = 1;
        else if (stat(ACC_OPENCL_TEMPDIR "/" ACC_OPENCL_CACHEDIR, &cachedir) == 0 && S_ISDIR(cachedir.st_mode)) cache = 2;
      }
      if (1 == cache) {
        static char neo_cachedir[] = "NEO_CACHE_DIR=" ACC_OPENCL_CACHEDIR;
        static char ocl_cachedir[] = "cl_cache_dir=" ACC_OPENCL_CACHEDIR;
        ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV(neo_cachedir)); /* putenv before entering OpenCL */
        ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV(ocl_cachedir)); /* putenv before entering OpenCL */
        env_cachedir = ACC_OPENCL_CACHEDIR;
      }
#    if defined(ACC_OPENCL_TEMPDIR)
      else if (NULL == env_cachedir) { /* code-path entered by default */
        if (NULL == env_cache || 0 != cache) { /* customize NEO_CACHE_DIR unless ACC_OPENCL_CACHE=0 */
          static char neo_cachedir[] = "NEO_CACHE_DIR=" ACC_OPENCL_TEMPDIR "/" ACC_OPENCL_CACHEDIR;
          ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV(neo_cachedir)); /* putenv before entering OpenCL */
          env_cachedir = ACC_OPENCL_TEMPDIR "/" ACC_OPENCL_CACHEDIR;
        }
        if (0 != cache) { /* legacy-NEO is treated with explicit opt-in */
          static char ocl_cachedir[] = "cl_cache_dir=" ACC_OPENCL_TEMPDIR "/" ACC_OPENCL_CACHEDIR;
          ACC_OPENCL_EXPECT(0 == LIBXSMM_PUTENV(ocl_cachedir)); /* putenv before entering OpenCL */
        }
      }
#    endif
      if (NULL != env_cachedir) {
#    if defined(_WIN32)
        LIBXSMM_UNUSED(env_cachedir);
#    else
#      if defined(S_IRWXU) && defined(S_IRGRP) && defined(S_IXGRP) && defined(S_IROTH) && defined(S_IXOTH)
        const int mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
#      else
        const int mode = 0xFFFFFFFF;
#      endif
        ACC_OPENCL_EXPECT(0 == mkdir(env_cachedir, mode) || EEXIST == errno); /* soft-error */
#    endif
      }
    }
#  endif
    if (CL_SUCCESS == clGetPlatformIDs(0, NULL, &nplatforms) && 0 < nplatforms) {
      ACC_OPENCL_CHECK(
        clGetPlatformIDs(nplatforms <= ACC_OPENCL_DEVICES_MAXCOUNT ? nplatforms : ACC_OPENCL_DEVICES_MAXCOUNT, platforms, 0),
        "retrieve platform ids", result);
    }
    if (EXIT_SUCCESS == result) {
      if (NULL != env_devtype && '\0' != *env_devtype) {
        if (NULL != LIBXSMM_STRISTR(env_devtype, "gpu")) {
          type = CL_DEVICE_TYPE_GPU;
        }
        else if (NULL != LIBXSMM_STRISTR(env_devtype, "cpu")) {
          type = CL_DEVICE_TYPE_CPU;
        }
        else if (NULL != LIBXSMM_STRISTR(env_devtype, "acc") || NULL != LIBXSMM_STRISTR(env_devtype, "other")) {
          type = CL_DEVICE_TYPE_ACCELERATOR;
        }
        else {
          type = CL_DEVICE_TYPE_ALL;
        }
      }
      c_dbcsr_acc_opencl_config.ndevices = 0;
      for (i = 0; i < nplatforms; ++i) {
        if (CL_SUCCESS == clGetDeviceIDs(platforms[i], type, 0, NULL, &ndevices) && 0 < ndevices) {
          ACC_OPENCL_CHECK(clGetDeviceIDs(platforms[i], type, ndevices, devices, NULL), "retrieve device ids", result);
          if (EXIT_SUCCESS == result) {
            cl_uint j = 0;
#  if defined(CL_VERSION_1_2)
            /* TODO: introduce more advanced syntax (partitioning a device) */
            const char* const env_devsplit = getenv("ACC_OPENCL_DEVSPLIT");
            const cl_uint devsplit = (NULL == env_devsplit ? 0 : atoi(env_devsplit));
            cl_uint n = 0;
#  endif
            for (; j < ndevices; ++j) {
#  if defined(CL_VERSION_1_2)
              cl_device_partition_property properties[] = {
                CL_DEVICE_PARTITION_BY_AFFINITY_DOMAIN, CL_DEVICE_AFFINITY_DOMAIN_NUMA, /*terminator*/ 0};
              cl_uint nunits = 0;
              if (0 != devsplit &&
                  CL_SUCCESS == clGetDeviceInfo(devices[j], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &nunits, NULL) &&
                  1 < nunits)
              {
                if (1 < devsplit) {
                  properties[0] = CL_DEVICE_PARTITION_EQUALLY;
                  properties[1] = (nunits + devsplit - 1) / devsplit;
                }
              }
              if ((NULL != env_devsplit && '0' == *env_devsplit) ||
                  (c_dbcsr_acc_opencl_config.ndevices + 1) == ACC_OPENCL_DEVICES_MAXCOUNT ||
                  (CL_SUCCESS != clCreateSubDevices(devices[j], properties, 0, NULL, &n)))
#  endif
              {
                c_dbcsr_acc_opencl_config.devices[c_dbcsr_acc_opencl_config.ndevices] = devices[j];
                ++c_dbcsr_acc_opencl_config.ndevices;
              }
#  if defined(CL_VERSION_1_2)
              else if (1 < n || 1 < nunits) { /* create subdevices */
                if (1 < nunits) {
                  properties[0] = CL_DEVICE_PARTITION_EQUALLY;
                  properties[1] = 1;
                  n = nunits;
                }
                if (ACC_OPENCL_DEVICES_MAXCOUNT < (c_dbcsr_acc_opencl_config.ndevices + n)) {
                  n = (cl_uint)ACC_OPENCL_DEVICES_MAXCOUNT - c_dbcsr_acc_opencl_config.ndevices;
                }
                if (EXIT_SUCCESS == clCreateSubDevices(devices[j], properties, n,
                                      c_dbcsr_acc_opencl_config.devices + c_dbcsr_acc_opencl_config.ndevices, NULL))
                {
                  ACC_OPENCL_CHECK(clReleaseDevice(devices[j]), "release device", result);
                  c_dbcsr_acc_opencl_config.ndevices += n;
                }
                else break;
              }
              else {
                c_dbcsr_acc_opencl_config.devices[c_dbcsr_acc_opencl_config.ndevices] = devices[j];
                ++c_dbcsr_acc_opencl_config.ndevices;
              }
#  endif
            }
          } /*else break;*/
        }
      }
    }
    if (EXIT_SUCCESS == result && 0 < c_dbcsr_acc_opencl_config.ndevices) {
      const char* const env_vendor = getenv("ACC_OPENCL_VENDOR");
      /* filter device by vendor (if requested) */
      if (NULL != env_vendor && '\0' != *env_vendor) {
        for (i = 0; (int)i < c_dbcsr_acc_opencl_config.ndevices;) {
          if (CL_SUCCESS ==
              clGetDeviceInfo(c_dbcsr_acc_opencl_config.devices[i], CL_DEVICE_VENDOR, ACC_OPENCL_BUFFERSIZE, buffer, NULL))
          {
            if (NULL == LIBXSMM_STRISTR(buffer, env_vendor)) {
#  if defined(CL_VERSION_1_2)
              ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseDevice(c_dbcsr_acc_opencl_config.devices[i]));
#  endif
              --c_dbcsr_acc_opencl_config.ndevices;
              if ((int)i < c_dbcsr_acc_opencl_config.ndevices) { /* keep original order (stable) */
                memmove(&c_dbcsr_acc_opencl_config.devices[i], &c_dbcsr_acc_opencl_config.devices[i + 1],
                  sizeof(cl_device_id) * (c_dbcsr_acc_opencl_config.ndevices - i));
              }
            }
            else ++i;
          }
          else break; /* error: retrieving device vendor */
        }
      }
      /* reorder devices according to c_dbcsr_acc_opencl_order_devices */
      if (EXIT_SUCCESS == result && 1 < c_dbcsr_acc_opencl_config.ndevices) {
        qsort(c_dbcsr_acc_opencl_config.devices, c_dbcsr_acc_opencl_config.ndevices, sizeof(cl_device_id),
          c_dbcsr_acc_opencl_order_devices);
      }
      /* ACC_OPENCL_DEVIDS is parsed as a list of devices (whitelist) */
      if (EXIT_SUCCESS == result && NULL != env_devids && '\0' != *env_devids) {
        cl_uint devids[ACC_OPENCL_DEVICES_MAXCOUNT], ndevids = 0;
        char* did = strtok(env_devids, ACC_OPENCL_DELIMS " ");
        for (; NULL != did && ndevids < ACC_OPENCL_DEVICES_MAXCOUNT; did = strtok(NULL, ACC_OPENCL_DELIMS " ")) {
          const int id = atoi(did);
          if (0 <= id && id < c_dbcsr_acc_opencl_config.ndevices) devids[ndevids++] = id;
        }
        if (0 < ndevids) {
          ndevices = (cl_uint)c_dbcsr_acc_opencl_config.ndevices;
          for (i = 0; i < ndevices; ++i) {
            cl_uint match = 0, j = 0;
            do
              if (i == devids[j]) {
                match = 1;
                break;
              }
            while (++j < ndevids);
            if (0 == match) {
#  if defined(CL_VERSION_1_2)
              ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseDevice(c_dbcsr_acc_opencl_config.devices[i]));
#  endif
              c_dbcsr_acc_opencl_config.devices[i] = NULL;
            }
          }
          for (i = c_dbcsr_acc_opencl_config.ndevices - 1;; --i) {
            if (NULL == c_dbcsr_acc_opencl_config.devices[i]) { /* keep original order (stable) */
              const cl_uint nmove = c_dbcsr_acc_opencl_config.ndevices - (i + 1);
              if (0 < nmove) {
                memmove(
                  &c_dbcsr_acc_opencl_config.devices[i], &c_dbcsr_acc_opencl_config.devices[i + 1], sizeof(cl_device_id) * nmove);
              }
              --c_dbcsr_acc_opencl_config.ndevices;
            }
            if (0 == i) break;
          }
        }
      }
    }
    if (EXIT_SUCCESS == result && 0 < c_dbcsr_acc_opencl_config.ndevices) {
      /* preselect any default device or prune to homogeneous set of devices */
      if (NULL == env_device || '\0' == *env_device) {
        char tmp[ACC_OPENCL_BUFFERSIZE] = "";
        ndevices = (cl_uint)c_dbcsr_acc_opencl_config.ndevices;
        for (i = 0; i < ndevices; ++i) {
          cl_device_type itype;
          result = clGetDeviceInfo(c_dbcsr_acc_opencl_config.devices[i], CL_DEVICE_TYPE, sizeof(cl_device_type), &itype, NULL);
          if (CL_SUCCESS == result) {
            if (0 != (CL_DEVICE_TYPE_DEFAULT & itype)) {
              if (0 < i) {
                c_dbcsr_acc_opencl_config.devices[0] = c_dbcsr_acc_opencl_config.devices[i];
              }
              c_dbcsr_acc_opencl_config.ndevices = 1;
              device_id = (int)i;
              break;
            }
            else if (CL_DEVICE_TYPE_ALL == type && NULL == env_devtype /*&& CL_DEVICE_TYPE_GPU == itype*/ && device_id <= (int)i) {
              result = clGetDeviceInfo(c_dbcsr_acc_opencl_config.devices[i], CL_DEVICE_NAME, ACC_OPENCL_BUFFERSIZE, buffer, NULL);
              if (CL_SUCCESS == result /* prune for homogeneous set of devices */
                  && ('\0' == *tmp || 0 == strncmp(buffer, tmp, ACC_OPENCL_BUFFERSIZE)))
              {
                c_dbcsr_acc_opencl_config.ndevices = i + 1;
                strncpy(tmp, buffer, ACC_OPENCL_BUFFERSIZE);
              }
              else break; /* error: retrieving device name */
            }
          }
          else break; /* error: retrieving device type */
        }
      }
      else { /* prune number of devices to only expose requested ID */
        if (1 < c_dbcsr_acc_opencl_config.ndevices) {
          if (0 < device_id) {
            c_dbcsr_acc_opencl_config.devices[0] =
              c_dbcsr_acc_opencl_config.devices[device_id % c_dbcsr_acc_opencl_config.ndevices];
          }
          c_dbcsr_acc_opencl_config.ndevices = 1;
        }
        device_id = 0;
      }
    }
    if (device_id < c_dbcsr_acc_opencl_config.ndevices) {
      if (EXIT_SUCCESS == result) {
        assert(NULL == c_dbcsr_acc_opencl_config.device && 0 < c_dbcsr_acc_opencl_config.ndevices);
        assert(c_dbcsr_acc_opencl_config.ndevices < ACC_OPENCL_DEVICES_MAXCOUNT);
        c_dbcsr_acc_opencl_config.device = (c_dbcsr_acc_opencl_device_t*)calloc(/* thread-specific */
          c_dbcsr_acc_opencl_config.nthreads, sizeof(c_dbcsr_acc_opencl_device_t));
        if (NULL != c_dbcsr_acc_opencl_config.device) {
          result = c_dbcsr_acc_opencl_set_active_device(/*main*/ 0, device_id);
          assert(EXIT_SUCCESS == result || NULL == c_dbcsr_acc_opencl_config.device[/*main*/ 0].context);
          if (1 < c_dbcsr_acc_opencl_config.verbosity || 0 > c_dbcsr_acc_opencl_config.verbosity) {
            char platform_name[ACC_OPENCL_BUFFERSIZE];
            for (i = 0; i < (cl_uint)c_dbcsr_acc_opencl_config.ndevices; ++i) {
              if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_name(c_dbcsr_acc_opencl_config.devices[i], buffer,
                                    ACC_OPENCL_BUFFERSIZE, platform_name, ACC_OPENCL_BUFFERSIZE, /*cleanup*/ 0))
              {
                fprintf(stderr, "INFO ACC/OpenCL: DEVICE -> \"%s : %s\"\n", platform_name, buffer);
              }
            }
          }
        }
        else {
          result = EXIT_FAILURE;
        }
        c_dbcsr_acc_opencl_config.nclmems = c_dbcsr_acc_opencl_config.nevents = 0;
        c_dbcsr_acc_opencl_config.clmems = c_dbcsr_acc_opencl_config.events = NULL;
        c_dbcsr_acc_opencl_config.storage = NULL;
#  if LIBXSMM_VERSION4(1, 17, 0, 0) < LIBXSMM_VERSION_NUMBER && defined(ACC_OPENCL_HANDLES_MAXCOUNT) && \
    (0 < ACC_OPENCL_HANDLES_MAXCOUNT)
        if (EXIT_SUCCESS == result) {
          const size_t nhandles = ACC_OPENCL_HANDLES_MAXCOUNT * c_dbcsr_acc_opencl_config.nthreads;
#    if defined(ACC_OPENCL_MEM_OFFSET)
          c_dbcsr_acc_opencl_config.nclmems = nhandles;
          c_dbcsr_acc_opencl_config.clmems = (void**)malloc(sizeof(void*) * nhandles);
          c_dbcsr_acc_opencl_config.storage = malloc(sizeof(void*) * (nhandles + nhandles));
          if (NULL != c_dbcsr_acc_opencl_config.clmems && NULL != c_dbcsr_acc_opencl_config.storage) {
            libxsmm_pmalloc_init(sizeof(void*), &c_dbcsr_acc_opencl_config.nclmems, c_dbcsr_acc_opencl_config.clmems,
              (void**)c_dbcsr_acc_opencl_config.storage + nhandles);
          }
          else {
            free(c_dbcsr_acc_opencl_config.clmems);
            c_dbcsr_acc_opencl_config.clmems = NULL;
            c_dbcsr_acc_opencl_config.nclmems = 0;
            result = EXIT_FAILURE;
          }
#    else
          c_dbcsr_acc_opencl_config.storage = malloc(sizeof(void*) * nhandles);
#    endif
          c_dbcsr_acc_opencl_config.nevents = nhandles;
          c_dbcsr_acc_opencl_config.events = (void**)malloc(sizeof(void*) * nhandles);
          if (NULL != c_dbcsr_acc_opencl_config.events && NULL != c_dbcsr_acc_opencl_config.storage) {
            libxsmm_pmalloc_init(sizeof(void*), &c_dbcsr_acc_opencl_config.nevents, c_dbcsr_acc_opencl_config.events,
              c_dbcsr_acc_opencl_config.storage);
          }
          else {
            free(c_dbcsr_acc_opencl_config.events);
            c_dbcsr_acc_opencl_config.events = NULL;
            c_dbcsr_acc_opencl_config.nevents = 0;
            result = EXIT_FAILURE;
          }
          if (EXIT_SUCCESS != result) {
            free(c_dbcsr_acc_opencl_config.storage);
            c_dbcsr_acc_opencl_config.storage = NULL;
          }
        }
#  endif
        if (EXIT_SUCCESS == result) {
          const int nelements = c_dbcsr_acc_opencl_config.nthreads * c_dbcsr_acc_opencl_config.nstreams;
          c_dbcsr_acc_opencl_config.streams = (void**)calloc(nelements, sizeof(void*)); /* allocate streams */
          if (NULL == c_dbcsr_acc_opencl_config.streams) result = EXIT_FAILURE;
        }
      }
    }
    else { /* mark as initialized */
      c_dbcsr_acc_opencl_config.ndevices = -1;
    }
#  if defined(__DBCSR_ACC)
    /* DBCSR shall call c_dbcsr_acc_init as well as libsmm_acc_init (since both interfaces are used).
     * Also, libsmm_acc_init may privately call c_dbcsr_acc_init (as it depends on the ACC interface).
     * The implementation of c_dbcsr_acc_init should hence be safe against "over initialization".
     * However, DBCSR only calls c_dbcsr_acc_init (and expects an implicit libsmm_acc_init).
     */
    if (EXIT_SUCCESS == result) result = libsmm_acc_init();
#  endif
  }
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  c_dbcsr_timestop(&routine_handle);
#  endif
  ACC_OPENCL_RETURN(result);
}


int c_dbcsr_acc_finalize(void) {
#  if defined(_OPENMP)
  /* initialization/finalization is not meant to be thread-safe */
  int result = ((0 == omp_in_parallel() || /*main*/ 0 == omp_get_thread_num()) ? EXIT_SUCCESS : EXIT_FAILURE);
#  else
  int result = EXIT_SUCCESS;
#  endif
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  int routine_handle;
  static const char* const routine_name_ptr = LIBXSMM_FUNCNAME;
  static const int routine_name_len = (int)sizeof(LIBXSMM_FUNCNAME) - 1;
  c_dbcsr_timeset((const char**)&routine_name_ptr, &routine_name_len, &routine_handle);
#  endif
  if (0 != c_dbcsr_acc_opencl_config.ndevices) {
    int i;
    assert(c_dbcsr_acc_opencl_config.ndevices < ACC_OPENCL_DEVICES_MAXCOUNT);
    if (0 != c_dbcsr_acc_opencl_config.verbosity) {
      cl_device_id device;
      int d;
      fprintf(stderr, "INFO ACC/OpenCL: pid=%u nthreads=%i", libxsmm_get_pid(), c_dbcsr_acc_opencl_config.nthreads);
      if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device(0, &device) &&
          EXIT_SUCCESS == c_dbcsr_acc_opencl_device_id(device, NULL /*devid*/, &d))
      {
        fprintf(stderr, " device=%i", d);
      }
      fprintf(stderr, "\n");
    }
#  if defined(__DBCSR_ACC)
    /* DBCSR may call c_dbcsr_acc_init as well as libsmm_acc_init() since both interface are used.
     * libsmm_acc_init may privately call c_dbcsr_acc_init (as it depends on the ACC interface).
     * The implementation of c_dbcsr_acc_init should be safe against "over initialization".
     * However, DBCSR only calls c_dbcsr_acc_init and expects an implicit libsmm_acc_init().
     */
    if (EXIT_SUCCESS == result) result = libsmm_acc_finalize();
#  endif
    libxsmm_finalize();
    if (NULL != c_dbcsr_acc_opencl_config.device) {
      for (i = 0; i < c_dbcsr_acc_opencl_config.nthreads; ++i) {
        const cl_context context = c_dbcsr_acc_opencl_config.device[i].context;
        if (NULL != context) {
          c_dbcsr_acc_opencl_config.device[i].context = NULL;
          clReleaseContext(context); /* ignore return code */
        }
      }
      free(c_dbcsr_acc_opencl_config.device); /* release buffer */
    }
    for (i = 0; i < ACC_OPENCL_DEVICES_MAXCOUNT; ++i) {
      const cl_device_id device_id = c_dbcsr_acc_opencl_config.devices[i];
      if (NULL != device_id) {
#  if defined(CL_VERSION_1_2) && defined(_DEBUG)
        ACC_OPENCL_CHECK(clReleaseDevice(device_id), "release device", result);
#  endif
        /* c_dbcsr_acc_opencl_create_context scans for non-NULL devices */
        c_dbcsr_acc_opencl_config.devices[i] = NULL;
      }
    }
    /* release/reset buffers */
    free(c_dbcsr_acc_opencl_config.events);
    free(c_dbcsr_acc_opencl_config.storage);
    free(c_dbcsr_acc_opencl_config.streams);
    /* clear configuration */
    memset(&c_dbcsr_acc_opencl_config, 0, sizeof(c_dbcsr_acc_opencl_config));
  }
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  c_dbcsr_timestop(&routine_handle);
#  endif
  ACC_OPENCL_RETURN(result);
}


void c_dbcsr_acc_clear_errors(void) {}


int c_dbcsr_acc_get_ndevices(int* ndevices) {
  int result;
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  int routine_handle;
  static const char* const routine_name_ptr = LIBXSMM_FUNCNAME;
  static const int routine_name_len = (int)sizeof(LIBXSMM_FUNCNAME) - 1;
  c_dbcsr_timeset((const char**)&routine_name_ptr, &routine_name_len, &routine_handle);
#  endif
#  if defined(ACC_OPENCL_LAZYINIT)
  /* DBCSR calls c_dbcsr_acc_get_ndevices before calling c_dbcsr_acc_init. */
  result = c_dbcsr_acc_init();
  if (EXIT_SUCCESS == result)
#  endif
  {
    if (NULL != ndevices && 0 != c_dbcsr_acc_opencl_config.ndevices) {
      *ndevices = (0 < c_dbcsr_acc_opencl_config.ndevices ? c_dbcsr_acc_opencl_config.ndevices : 0);
      result = EXIT_SUCCESS;
    }
    else result = EXIT_FAILURE;
  }
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  c_dbcsr_timestop(&routine_handle);
#  endif
  ACC_OPENCL_RETURN(result);
}


int c_dbcsr_acc_opencl_device(int thread_id, cl_device_id* device) {
  int result = EXIT_SUCCESS;
  assert(0 <= thread_id && thread_id < c_dbcsr_acc_opencl_config.nthreads);
  assert(NULL != device);
  if (NULL != c_dbcsr_acc_opencl_config.device) {
    cl_context context = c_dbcsr_acc_opencl_config.device[thread_id].context;
#  if defined(_OPENMP)
    if (NULL == context && 0 < thread_id) { /* fallback to master's context */
      context = c_dbcsr_acc_opencl_config.device[/*main*/ 0].context;
    }
#  endif
    if (NULL != context) {
      result = clGetContextInfo(context, CL_CONTEXT_DEVICES, sizeof(cl_device_id), device, NULL);
    }
    else *device = NULL;
  }
  else *device = NULL;
  return result;
}


int c_dbcsr_acc_opencl_device_id(cl_device_id device, int* device_id, int* global_id) {
  int result = EXIT_SUCCESS, i;
  assert(c_dbcsr_acc_opencl_config.ndevices < ACC_OPENCL_DEVICES_MAXCOUNT);
  assert(NULL != device_id || NULL != global_id);
  for (i = 0; i < c_dbcsr_acc_opencl_config.ndevices; ++i) {
    if (device == c_dbcsr_acc_opencl_config.devices[i]) break;
  }
  if (i < c_dbcsr_acc_opencl_config.ndevices) {
    if (NULL != device_id) *device_id = i;
    if (NULL != global_id) {
      *global_id = i;
      for (++i; i < ACC_OPENCL_DEVICES_MAXCOUNT; ++i) {
        if (NULL != c_dbcsr_acc_opencl_config.devices[i]) {
          if (device == c_dbcsr_acc_opencl_config.devices[i]) {
            *global_id = i;
            break;
          }
        }
        else break;
      }
    }
  }
  else {
    if (NULL != device_id) *device_id = -1;
    if (NULL != global_id) *global_id = -1;
    if (NULL != device) result = EXIT_FAILURE;
  }
  return result;
}


int c_dbcsr_acc_opencl_device_vendor(cl_device_id device, const char vendor[], int use_platform_name) {
  char buffer[ACC_OPENCL_BUFFERSIZE];
  int result = EXIT_SUCCESS;
  assert(NULL != device && NULL != vendor);
  if (0 == use_platform_name) {
    ACC_OPENCL_CHECK(
      clGetDeviceInfo(device, CL_DEVICE_VENDOR, ACC_OPENCL_BUFFERSIZE, buffer, NULL), "retrieve device vendor", result);
  }
  else {
    cl_platform_id platform_id;
    ACC_OPENCL_CHECK(
      clGetDeviceInfo(device, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform_id, NULL), "retrieve platform id", result);
    if (EXIT_SUCCESS == result) {
      ACC_OPENCL_CHECK(
        clGetPlatformInfo(platform_id, CL_PLATFORM_NAME, ACC_OPENCL_BUFFERSIZE, buffer, NULL), "retrieve platform name", result);
    }
  }
  if (EXIT_SUCCESS == result) {
    result = (NULL != LIBXSMM_STRISTR(buffer, vendor) ? EXIT_SUCCESS : EXIT_FAILURE);
  }
  return result;
}


int c_dbcsr_acc_opencl_device_uid(cl_device_id device, const char devname[], unsigned int* uid) {
  int result;
  if (NULL != uid) {
    if (NULL != device && EXIT_SUCCESS == c_dbcsr_acc_opencl_device_vendor(device, "intel", 0 /*use_platform_name*/)) {
      result = clGetDeviceInfo(device, 0x4251 /*CL_DEVICE_ID_INTEL*/, sizeof(unsigned int), uid, NULL);
    }
    else result = EXIT_FAILURE;
    if (EXIT_SUCCESS != result) {
      if (NULL != devname && '\0' != *devname) {
        *uid = (unsigned int)strtoul(devname, NULL, 0);
        if (0 == *uid) {
          const char *const begin = strrchr(devname, '['), *const end = strrchr(devname, ']');
          if (NULL != begin && begin < end) {
            *uid = (unsigned int)strtoul(begin + 1, NULL, 0);
          }
          if (0 == *uid) {
            const size_t size = strlen(devname);
            const unsigned int hash = libxsmm_hash(devname, (unsigned int)size, 25071975 /*seed*/);
            *uid = libxsmm_hash(&hash, 4 /*size*/, hash >> 16 /*seed*/) & 0xFFFF;
          }
        }
        result = EXIT_SUCCESS;
      }
      else {
        result = EXIT_FAILURE;
        *uid = 0;
      }
    }
  }
  else result = EXIT_FAILURE;
  return result;
}


int c_dbcsr_acc_opencl_device_name(
  cl_device_id device, char name[], size_t name_maxlen, char platform[], size_t platform_maxlen, int cleanup) {
  int result_name = 0, result_platform = 0;
  assert(NULL != name || NULL != platform);
  if (NULL != name && 0 != name_maxlen) {
    result_name = clGetDeviceInfo(device, CL_DEVICE_NAME, name_maxlen, name, NULL);
    if (0 != cleanup && CL_SUCCESS == result_name) {
      char* const part = strchr(name, ':');
      if (NULL != part) *part = '\0';
    }
  }
  if (NULL != platform && 0 != platform_maxlen) {
    cl_platform_id platform_id;
    result_platform = clGetDeviceInfo(device, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform_id, NULL);
    if (CL_SUCCESS == result_platform) {
      result_platform = clGetPlatformInfo(platform_id, CL_PLATFORM_NAME, platform_maxlen, platform, NULL);
    }
  }
  return result_name | result_platform;
}


int c_dbcsr_acc_opencl_device_level(
  cl_device_id device, int* level_major, int* level_minor, char cl_std[16], cl_device_type* type) {
  cl_int result = EXIT_SUCCESS;
  assert(NULL != device && (NULL != level_major || NULL != level_minor || NULL != cl_std || NULL != type));
  if (NULL != level_major || NULL != level_minor || NULL != cl_std) {
    char buffer[ACC_OPENCL_BUFFERSIZE];
    result = clGetDeviceInfo(device, CL_DEVICE_VERSION, ACC_OPENCL_BUFFERSIZE, buffer, NULL);
    if (CL_SUCCESS == result) {
      unsigned int cl_std_level[2];
      if (2 == sscanf(buffer, "OpenCL %u.%u", cl_std_level, cl_std_level + 1)) {
        if (NULL != level_major) *level_major = (int)cl_std_level[0];
        if (NULL != level_minor) *level_minor = (int)cl_std_level[1];
        if (NULL != cl_std) {
          if (2 <= cl_std_level[0]) {
            const int nchar = LIBXSMM_SNPRINTF(cl_std, 16, "-cl-std=CL%u.0", cl_std_level[0]);
            if (0 >= nchar || 16 <= nchar) result = EXIT_FAILURE;
          }
          else if (1 <= cl_std_level[0]) {
            if (1 <= cl_std_level[1]) {
              const int nchar = LIBXSMM_SNPRINTF(cl_std, 16, "-cl-std=CL%u.%u", cl_std_level[0], cl_std_level[1]);
              if (0 >= nchar || 16 <= nchar) result = EXIT_FAILURE;
            }
            else {
              result = clGetDeviceInfo(device, CL_DEVICE_OPENCL_C_VERSION, ACC_OPENCL_BUFFERSIZE, buffer, NULL);
              if (CL_SUCCESS == result) {
                if (2 == sscanf(buffer, "OpenCL C %u.%u", cl_std_level, cl_std_level + 1)) {
                  const int nchar = LIBXSMM_SNPRINTF(cl_std, 16, "-cl-std=CL%u.%u", cl_std_level[0], cl_std_level[1]);
                  if (0 >= nchar || 16 <= nchar) result = EXIT_FAILURE;
                }
                else {
                  result = EXIT_FAILURE;
                  *cl_std = '\0';
                }
              }
              else *cl_std = '\0';
            }
          }
          else *cl_std = '\0';
        }
      }
      else {
        if (NULL != level_major) *level_major = 0;
        if (NULL != level_minor) *level_minor = 0;
        if (NULL != cl_std) *cl_std = '\0';
        result = EXIT_FAILURE;
      }
    }
    else {
      if (NULL != level_major) *level_major = 0;
      if (NULL != level_minor) *level_minor = 0;
      if (NULL != cl_std) *cl_std = '\0';
    }
  }
  if (NULL != type && EXIT_SUCCESS == result) {
    result = clGetDeviceInfo(device, CL_DEVICE_TYPE, sizeof(cl_device_type), type, NULL);
  }
  return result;
}


int c_dbcsr_acc_opencl_device_ext(cl_device_id device, const char* const extnames[], int num_exts) {
  int result = ((NULL != extnames && 0 < num_exts) ? EXIT_SUCCESS : EXIT_FAILURE);
  char extensions[ACC_OPENCL_BUFFERSIZE], buffer[ACC_OPENCL_BUFFERSIZE];
  assert(NULL != device);
  ACC_OPENCL_CHECK(
    clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, ACC_OPENCL_BUFFERSIZE, extensions, NULL), "retrieve device extensions", result);
  if (EXIT_SUCCESS == result) {
    do {
      if (NULL != extnames[--num_exts]) {
        const char* const end = buffer + strlen(extnames[num_exts]); /* before strtok */
        char* ext = strtok(strncpy(buffer, extnames[num_exts], ACC_OPENCL_BUFFERSIZE - 1), ACC_OPENCL_DELIMS " \t");
        for (; NULL != ext; ext = ((ext + 1) < end ? strtok((ext + 1) + strlen(ext), ACC_OPENCL_DELIMS " \t") : NULL)) {
          if (NULL == strstr(extensions, ext)) {
            return EXIT_FAILURE;
          }
        }
      }
    } while (0 < num_exts);
  }
  return result;
}


int c_dbcsr_acc_opencl_create_context(int thread_id, cl_device_id active_id) {
  cl_platform_id platform = NULL;
  cl_int result;
  assert(0 <= thread_id && thread_id < c_dbcsr_acc_opencl_config.nthreads);
  assert(NULL == c_dbcsr_acc_opencl_config.device[thread_id].context);
  assert(0 < c_dbcsr_acc_opencl_config.ndevices);
  assert(NULL != active_id);
  result = clGetDeviceInfo(active_id, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform, NULL);
  assert(CL_SUCCESS != result || NULL != platform);
  if (CL_SUCCESS == result) {
    void (*const notify)(
      const char*, const void*, size_t, void*) = (0 != c_dbcsr_acc_opencl_config.verbosity ? c_dbcsr_acc_opencl_notify : NULL);
    cl_context_properties properties[] = {
      CL_CONTEXT_PLATFORM, 0 /*placeholder*/, 0 /* end of properties */
    };
    cl_context context = NULL;
    properties[1] = (long)platform;
    context = clCreateContext(properties, 1 /*num_devices*/, &active_id, notify, NULL /* user_data*/, &result);
    if (CL_SUCCESS != result && CL_INVALID_DEVICE != result) { /* retry */
      context = clCreateContext(NULL /*properties*/, 1 /*num_devices*/, &active_id, notify, NULL /* user_data*/, &result);
    }
    if (CL_SUCCESS == result) {
      assert(NULL != context);
      c_dbcsr_acc_opencl_config.device[thread_id].context = context;
      if (0 != thread_id) {
        /* apply context to master-thread if master's context is NULL */
        LIBXSMM_ATOMIC_CMPSWP(&c_dbcsr_acc_opencl_config.device[/*main*/ 0].context, NULL, context, LIBXSMM_ATOMIC_RELAXED);
        assert(NULL != c_dbcsr_acc_opencl_config.device[/*main*/ 0].context);
      }
      if (0 != c_dbcsr_acc_opencl_config.verbosity) {
        char buffer[ACC_OPENCL_BUFFERSIZE];
        int global_id = 0;
        if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_name(
                              active_id, buffer, ACC_OPENCL_BUFFERSIZE, NULL /*platform*/, 0 /*platform_maxlen*/, /*cleanup*/ 1) &&
            EXIT_SUCCESS == c_dbcsr_acc_opencl_device_id(active_id, NULL /*devid*/, &global_id))
        {
          const size_t size = strlen(buffer);
          unsigned int uid[] = {0, 0};
          if ((EXIT_SUCCESS == c_dbcsr_acc_opencl_device_uid(NULL /*device*/, buffer, uid + 1)) &&
              (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_uid(active_id, NULL /*devname*/, uid) || 0 != uid[1]) && uid[0] != uid[1])
          {
            ACC_OPENCL_EXPECT(0 < LIBXSMM_SNPRINTF(buffer + size, LIBXSMM_MAX(0, ACC_OPENCL_BUFFERSIZE - size), " [0x%04x]",
                                    0 != uid[0] ? uid[0] : uid[1]));
          }
          fprintf(stderr, "INFO ACC/OpenCL: ndevices=%i device%i=\"%s\"\n", c_dbcsr_acc_opencl_config.ndevices, global_id, buffer);
        }
      }
    }
    else if (CL_INVALID_DEVICE == result &&
             EXIT_SUCCESS == c_dbcsr_acc_opencl_device_vendor(active_id, "nvidia", 0 /*use_platform_name*/))
    {
      fprintf(stderr, "WARN ACC/OpenCL: if MPI-ranks target the same device in exclusive mode,\n"
                      "                    SMI must be used to enable sharing the device.\n");
    }
  }
  return result;
}


int c_dbcsr_acc_opencl_set_active_device(int thread_id, int device_id) {
  int result = EXIT_SUCCESS;
  cl_device_id active_id;
  assert(0 <= thread_id && thread_id < c_dbcsr_acc_opencl_config.nthreads);
  assert(c_dbcsr_acc_opencl_config.ndevices < ACC_OPENCL_DEVICES_MAXCOUNT);
  if (0 <= device_id && device_id < c_dbcsr_acc_opencl_config.ndevices) {
    assert(NULL != c_dbcsr_acc_opencl_config.device);
    active_id = c_dbcsr_acc_opencl_config.devices[device_id];
    if (NULL != active_id) {
#  if defined(_OPENMP)
#    pragma omp critical(c_dbcsr_acc_set_active_device)
#  endif
      {
        int inherit_id = 0;
        const cl_context context = c_dbcsr_acc_opencl_device_context(active_id, &inherit_id);
        const cl_context inherit = c_dbcsr_acc_opencl_config.device[inherit_id].context;
        if (NULL != context) {
          if (context != inherit) {
            if (NULL != inherit) {
              c_dbcsr_acc_opencl_config.device[inherit_id].context = NULL;
              result = clReleaseContext(inherit);
            }
            else if (thread_id != inherit_id) {
              c_dbcsr_acc_opencl_config.device[inherit_id].context = context;
              result = clRetainContext(context);
            }
          }
        }
        else if (NULL == c_dbcsr_acc_opencl_config.device[thread_id].context) {
          result = c_dbcsr_acc_opencl_create_context(thread_id, active_id);
          if (EXIT_SUCCESS == result && NULL /*context*/ != inherit) {
            c_dbcsr_acc_opencl_config.device[inherit_id].context = c_dbcsr_acc_opencl_config.device[thread_id].context;
            result = clReleaseContext(inherit);
          }
        }
        if (EXIT_SUCCESS == result) { /* update/cache device-specific information */
          result = c_dbcsr_acc_opencl_device_level(active_id, c_dbcsr_acc_opencl_config.device[thread_id].level,
            c_dbcsr_acc_opencl_config.device[thread_id].level + 1, NULL /*cl_std*/,
            &c_dbcsr_acc_opencl_config.device[thread_id].type);
          if (EXIT_SUCCESS == result) {
            char devname[ACC_OPENCL_BUFFERSIZE];
#  if defined(CL_VERSION_2_0)
            const char* const env_svm = getenv("ACC_OPENCL_SVM");
            c_dbcsr_acc_opencl_config.device[thread_id].svm_interop =
              ((NULL == env_svm || 2 > *c_dbcsr_acc_opencl_config.device[thread_id].level) ? 0 : atoi(env_svm));
#  endif
            if (CL_SUCCESS != clGetDeviceInfo(active_id, CL_DEVICE_HOST_UNIFIED_MEMORY, sizeof(cl_bool),
                                &c_dbcsr_acc_opencl_config.device[thread_id].unified, NULL))
            {
              c_dbcsr_acc_opencl_config.device[thread_id].unified = CL_FALSE;
            }
            if (EXIT_SUCCESS != c_dbcsr_acc_opencl_device_name(active_id, devname, ACC_OPENCL_BUFFERSIZE, NULL /*platform*/,
                                  0 /*platform_maxlen*/, /*cleanup*/ 1) ||
                EXIT_SUCCESS != c_dbcsr_acc_opencl_device_uid(active_id, devname, &c_dbcsr_acc_opencl_config.device[thread_id].uid))
            {
              c_dbcsr_acc_opencl_config.device[thread_id].uid = (cl_uint)-1;
            }
            c_dbcsr_acc_opencl_config.device[thread_id].intel = (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_vendor(
                                                                                   active_id, "intel", 0 /*use_platform_name*/));
            c_dbcsr_acc_opencl_config.device[thread_id].nv = (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_vendor(
                                                                                active_id, "nvidia", 0 /*use_platform_name*/));
            if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_vendor(active_id, "amd", 0 /*use_platform_name*/) ||
                EXIT_SUCCESS == c_dbcsr_acc_opencl_device_vendor(active_id, "amd", 1 /*use_platform_name*/))
            {
              char buffer[ACC_OPENCL_BUFFERSIZE];
              c_dbcsr_acc_opencl_config.device[thread_id].amd = 1;
              if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_name(active_id, buffer, ACC_OPENCL_BUFFERSIZE, NULL /*platform*/,
                                    0 /*platform_maxlen*/, /*cleanup*/ 1))
              {
                const char* const gfxname = LIBXSMM_STRISTR(buffer, "gfx");
                if (NULL != gfxname && 90 <= atoi(gfxname + 3)) {
                  c_dbcsr_acc_opencl_config.device[thread_id].amd = 2;
                }
              }
            }
          }
        }
      }
    }
    else result = EXIT_FAILURE;
  }
  return result;
}


int c_dbcsr_acc_set_active_device(int device_id) {
  int result;
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  int routine_handle;
  static const char* const routine_name_ptr = LIBXSMM_FUNCNAME;
  static const int routine_name_len = (int)sizeof(LIBXSMM_FUNCNAME) - 1;
  c_dbcsr_timeset((const char**)&routine_name_ptr, &routine_name_len, &routine_handle);
#  endif
  assert(0 != c_dbcsr_acc_opencl_config.ndevices);
  result = c_dbcsr_acc_opencl_set_active_device(ACC_OPENCL_OMP_TID(), device_id);
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  c_dbcsr_timestop(&routine_handle);
#  endif
  ACC_OPENCL_RETURN(result);
}


int c_dbcsr_acc_opencl_device_synchronize(int thread_id) {
  void** const streams = c_dbcsr_acc_opencl_config.streams + thread_id * c_dbcsr_acc_opencl_config.nstreams;
  int result = EXIT_SUCCESS;
  int i = 0;
  assert(0 <= thread_id && thread_id < c_dbcsr_acc_opencl_config.nthreads);
  assert(NULL != c_dbcsr_acc_opencl_config.streams);
  for (; i < c_dbcsr_acc_opencl_config.nstreams; ++i) {
    void* const stream = streams[i];
    if (NULL != stream) {
      if (NULL != *ACC_OPENCL_STREAM(stream)) { /* soft-error? */
        result = c_dbcsr_acc_stream_sync(stream);
        if (EXIT_SUCCESS != result) break;
      }
    }
#  if defined(ACC_OPENCL_STREAM_COMPACT)
    else break;
#  endif
  }
  return result;
}


int c_dbcsr_acc_device_synchronize(void) {
  int result = EXIT_SUCCESS;
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  int routine_handle;
  static const char* const routine_name_ptr = LIBXSMM_FUNCNAME;
  static const int routine_name_len = (int)sizeof(LIBXSMM_FUNCNAME) - 1;
  c_dbcsr_timeset((const char**)&routine_name_ptr, &routine_name_len, &routine_handle);
#  endif
#  if defined(_OPENMP)
  if (1 < omp_get_num_threads()) {
    result = c_dbcsr_acc_opencl_device_synchronize(omp_get_thread_num());
  }
  else {
    int i;
#    pragma omp parallel for private(i)
    for (i = 0; i < c_dbcsr_acc_opencl_config.nthreads; ++i) {
      ACC_OPENCL_EXPECT(EXIT_SUCCESS == c_dbcsr_acc_opencl_device_synchronize(i));
    }
  }
#  else
  result = c_dbcsr_acc_opencl_device_synchronize(/*main*/ 0);
#  endif
#  if defined(__DBCSR_ACC) && defined(ACC_OPENCL_PROFILE)
  c_dbcsr_timestop(&routine_handle);
#  endif
  ACC_OPENCL_RETURN(result);
}


int c_dbcsr_acc_opencl_wgsize(cl_device_id device, cl_kernel kernel, size_t* max_value, size_t* preferred_multiple) {
  int result = (NULL != device && (NULL != preferred_multiple || NULL != max_value)) ? EXIT_SUCCESS : EXIT_FAILURE;
  if (NULL != kernel) { /* kernel-specific */
    if (NULL != max_value) {
      ACC_OPENCL_CHECK(clGetKernelWorkGroupInfo(kernel, device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), max_value, NULL),
        "query maximum WG-size of kernel", result);
    }
    if (NULL != preferred_multiple) {
      ACC_OPENCL_CHECK(clGetKernelWorkGroupInfo(
                         kernel, device, CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE, sizeof(size_t), preferred_multiple, NULL),
        "query preferred multiple of WG-size of kernel", result);
    }
  }
  else { /* device-specific */
    if (NULL != max_value) {
      ACC_OPENCL_CHECK(clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(size_t), max_value, NULL),
        "query maximum WG-size of device", result);
    }
    if (NULL != preferred_multiple) {
#  if defined(CL_VERSION_3_0)
      ACC_OPENCL_CHECK(
        clGetDeviceInfo(device, CL_DEVICE_PREFERRED_WORK_GROUP_SIZE_MULTIPLE, sizeof(size_t), preferred_multiple, NULL),
        "query preferred multiple of WG-size of device", result);
#  else
      *preferred_multiple = 1;
#  endif
    }
  }
  return result;
}


int c_dbcsr_acc_opencl_flags_atomics(cl_device_id device_id, c_dbcsr_acc_opencl_atomic_fp_t kind,
  const c_dbcsr_acc_opencl_device_t* devinfo, const char* exts[], int exts_maxlen, char flags[], size_t flags_maxlen) {
  int result = 0, ext1, ext2;
  for (ext1 = 0; ext1 < exts_maxlen; ++ext1)
    if (NULL == exts[ext1] || '\0' == *exts[ext1]) break;
  for (ext2 = ext1 + 1; ext2 < exts_maxlen; ++ext2)
    if (NULL == exts[ext2] || '\0' == *exts[ext2]) break;
  if (NULL != devinfo && ext2 < exts_maxlen) {
    const char* atomic_type = "";
    switch (kind) {
      case c_dbcsr_acc_opencl_atomic_fp_64: {
        exts[ext1] = "cl_khr_fp64 cl_khr_int64_base_atomics cl_khr_int64_extended_atomics";
        if (2 <= *devinfo->level && EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(device_id, exts, ext2)) {
          atomic_type = "-DTA=long -DTA2=atomic_long -DTF=atomic_double";
        }
        else {
          exts[ext1] = "cl_khr_fp64 cl_khr_int64_base_atomics";
          if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(device_id, exts, ext2)) {
            atomic_type = "-DTA=long";
          }
          else { /* fallback */
            exts[ext1] = "cl_khr_fp64 cl_khr_global_int32_base_atomics cl_khr_global_int32_extended_atomics";
            if (2 <= *devinfo->level && EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(device_id, exts, ext2)) {
              atomic_type = "-DATOMIC32_ADD64 -DTA=int -DTA2=atomic_int -DTF=atomic_double";
            }
            else {
              exts[ext1] = "cl_khr_fp64 cl_khr_global_int32_base_atomics";
              if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(device_id, exts, ext2)) {
                atomic_type = "-DATOMIC32_ADD64 -DTA=int";
              }
              else kind = c_dbcsr_acc_opencl_atomic_fp_no;
            }
          }
        }
      } break;
      case c_dbcsr_acc_opencl_atomic_fp_32: {
        exts[ext1] = "cl_khr_global_int32_base_atomics cl_khr_global_int32_extended_atomics";
        if (2 <= *devinfo->level && EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(device_id, exts, ext2)) {
          exts[ext2] = "cl_khr_int64_base_atomics cl_khr_int64_extended_atomics";
          atomic_type = "-DTA=int -DTA2=atomic_int -DTF=atomic_float";
        }
        else {
          exts[ext1] = "cl_khr_global_int32_base_atomics";
          if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(device_id, exts, ext2)) {
            exts[ext2] = "cl_khr_int64_base_atomics";
            atomic_type = "-DTA=int";
          }
          else kind = c_dbcsr_acc_opencl_atomic_fp_no;
        }
      } break;
      default: assert(c_dbcsr_acc_opencl_atomic_fp_no == kind);
    }
    if (c_dbcsr_acc_opencl_atomic_fp_no != kind) {
      const char *barrier_expr = NULL, *atomic_exp = NULL, *atomic_ops = "";
      const char* const env_barrier = getenv("ACC_OPENCL_BARRIER");
      const char* const env_atomics = getenv("ACC_OPENCL_ATOMICS");
      if (NULL == env_barrier || '0' != *env_barrier) {
        barrier_expr = ((2 <= *devinfo->level && (0 == devinfo->intel || (CL_DEVICE_TYPE_CPU != devinfo->type)))
                          ? "-D\"BARRIER(A)=work_group_barrier(A,memory_scope_work_group)\""
                          : "-D\"BARRIER(A)=barrier(A)\"");
      }
      else barrier_expr = ""; /* no barrier */
      assert(NULL != barrier_expr);
      if (NULL == env_atomics || '0' != *env_atomics) {
        /* can signal/force atomics without confirmation */
        const int force_atomics = ((NULL == env_atomics || '\0' == *env_atomics) ? 0 : atoi(env_atomics));
        if (NULL == env_atomics || '\0' == *env_atomics || 0 != force_atomics) {
          cl_bitfield fp_atomics;
          if (CL_SUCCESS == clGetDeviceInfo(device_id, (cl_device_info)(c_dbcsr_acc_opencl_atomic_fp_64 == kind ? 0x4232 : 0x4231),
                              sizeof(cl_bitfield), &fp_atomics, NULL) &&
              0 != (/*add*/ (1 << 1) & fp_atomics))
          {
            exts[ext2] = "cl_ext_float_atomics";
            atomic_exp = (c_dbcsr_acc_opencl_atomic_fp_64 == kind
                            ? "atomic_fetch_add_explicit((GLOBAL_VOLATILE(atomic_double)*)A,B,"
                              "memory_order_relaxed,memory_scope_work_group)"
                            : "atomic_fetch_add_explicit((GLOBAL_VOLATILE(atomic_float)*)A,B,"
                              "memory_order_relaxed,memory_scope_work_group)");
          }
          else if (0 != force_atomics || (0 != devinfo->intel && ((0x4905 != devinfo->uid && 0 == devinfo->unified)))) {
            if ((((0 != force_atomics || (0 != devinfo->intel && ((0x0bd0 <= devinfo->uid && 0x0bdb >= devinfo->uid) ||
                                                                   c_dbcsr_acc_opencl_atomic_fp_32 == kind))))))
            {
              if (0 == force_atomics && (0 == devinfo->intel || 0x0bd0 > devinfo->uid || 0x0bdb < devinfo->uid)) {
                exts[ext2] = "cl_intel_global_float_atomics";
                atomic_ops = "-Dcl_intel_global_float_atomics";
              }
              else {
                atomic_ops = ((2 > *devinfo->level && 2 > force_atomics)
                                ? "-DATOMIC_PROTOTYPES=1"
                                : (3 > force_atomics ? "-DATOMIC_PROTOTYPES=2" : "-DATOMIC_PROTOTYPES=3"));
              }
              atomic_exp = ((2 > *devinfo->level && 2 > force_atomics) ? "atomic_add(A,B)"
                                                                       : "atomic_fetch_add_explicit((GLOBAL_VOLATILE(TF)*)A,B,"
                                                                         "memory_order_relaxed,memory_scope_work_group)");
            }
            else {
              atomic_exp = "atomic_add_global_cmpxchg(A,B)";
              atomic_ops = "-DCMPXCHG=atom_cmpxchg";
            }
          }
          else if (0 == devinfo->nv) {
            if (1 >= devinfo->amd) {
              atomic_ops = (c_dbcsr_acc_opencl_atomic_fp_32 == kind ? "-DCMPXCHG=atomic_cmpxchg" : "-DCMPXCHG=atom_cmpxchg");
              atomic_exp = "atomic_add_global_cmpxchg(A,B)";
              exts[ext2] = NULL;
            }
            else { /* GCN */
              atomic_exp = (c_dbcsr_acc_opencl_atomic_fp_64 == kind
                              ? "__builtin_amdgcn_global_atomic_fadd_f64(A,B,__ATOMIC_RELAXED)"
                              : "__builtin_amdgcn_global_atomic_fadd_f32(A,B,__ATOMIC_RELAXED)");
            }
          }
          else { /* xchg */
            assert(NULL != atomic_ops && '\0' == *atomic_ops);
            atomic_exp = "atomic_add_global_xchg(A,B)";
          }
        }
        else if (NULL != LIBXSMM_STRISTR(env_atomics, "cmpxchg")) {
          atomic_ops = (c_dbcsr_acc_opencl_atomic_fp_32 == kind ? "-DCMPXCHG=atomic_cmpxchg" : "-DCMPXCHG=atom_cmpxchg");
          atomic_exp = "atomic_add_global_cmpxchg(A,B)";
          exts[ext2] = NULL;
        }
        else { /* xchg */
          atomic_exp = "atomic_add_global_xchg(A,B)";
          atomic_ops = (c_dbcsr_acc_opencl_atomic_fp_32 == kind ? "-DXCHG=atomic_xchg" : "-DXCHG=atom_xchg");
        }
      }
      else { /* unsynchronized */
        atomic_exp = "*(A)+=(B)"; /* non-atomic update */
      }
      assert(NULL != atomic_exp);
      /* compose build parameters and flags */
      result = LIBXSMM_SNPRINTF(flags, flags_maxlen, "-DTAN=%i %s %s -D\"ATOMIC_ADD_GLOBAL(A,B)=%s\" %s", kind, atomic_type,
        atomic_ops, atomic_exp, barrier_expr);
    }
  }
  return result;
}


int c_dbcsr_acc_opencl_flags(const char build_params[], const char build_options[], const char try_build_options[],
  const char cl_std[], char buffer[], size_t buffer_size) {
  int result;
  if (NULL != buffer) {
    const int nchar = LIBXSMM_SNPRINTF(buffer, buffer_size, "%s %s %s %s", NULL != cl_std ? cl_std : "",
      NULL != build_options ? build_options : "", NULL != build_params ? build_params : "",
      NULL != try_build_options ? try_build_options : "");
    if (0 < nchar && (int)buffer_size > nchar) {
      char* replace = strpbrk(buffer, "\""); /* more portable (system/cpp needs quotes to protect braces) */
      for (; NULL != replace; replace = strpbrk(replace + 1, "\"")) *replace = ' ';
      result = EXIT_SUCCESS;
    }
    else {
      result = EXIT_FAILURE;
      *buffer = '\0';
    }
  }
  else result = EXIT_FAILURE;
  return result;
}


int c_dbcsr_acc_opencl_kernel(int source_is_file, const char source[], const char kernel_name[], const char build_params[],
  const char build_options[], const char try_build_options[], int* try_ok, const char* const extnames[], int num_exts,
  cl_kernel* kernel) {
  char buffer[ACC_OPENCL_BUFFERSIZE] = "", cl_std[16];
  char buffer_name[ACC_OPENCL_MAXSTRLEN * 2];
  int tid = 0, ok = EXIT_SUCCESS, source_is_cl = 1, nchar, level_major, level_minor;
  const cl_context context = c_dbcsr_acc_opencl_context(&tid);
  cl_device_id active_id = NULL;
  cl_int result = ((NULL != source && NULL != kernel_name && '\0' != *kernel_name && NULL != kernel)
                     ? c_dbcsr_acc_opencl_device(tid, &active_id)
                     : EXIT_FAILURE);
  cl_program program = NULL;
  FILE* file_src = NULL;
  size_t size_src = 0;
  if (EXIT_SUCCESS == result) {
    result = c_dbcsr_acc_opencl_device_level(active_id, &level_major, &level_minor, cl_std, NULL /*type*/);
    if (0 != source_is_file) file_src = fopen(source, "rb");
  }
  if (NULL != file_src) {
    if (EXIT_SUCCESS == result) {
      const char* const file_ext = strrchr(source, '.');
      char* src = NULL;
      source_is_cl = ((NULL != file_ext && NULL != LIBXSMM_STRISTR(file_ext + 1, "cl")) ? 1 : 0);
      size_src = (EXIT_SUCCESS == fseek(file_src, 0 /*offset*/, SEEK_END) ? ftell(file_src) : 0);
      src = (char*)((0 != size_src && EXIT_SUCCESS == fseek(file_src, 0 /*offset*/, SEEK_SET))
                      ? libxsmm_aligned_scratch(size_src + source_is_cl /*terminator?*/, 0 /*auto-align*/)
                      : NULL);
      if (NULL != src) {
        if (size_src == fread(src, 1 /*sizeof(char)*/, size_src /*count*/, file_src)) {
          if (0 != source_is_cl) src[size_src] = '\0'; /* terminator */
          source = src;
        }
        else {
          result = EXIT_FAILURE;
          libxsmm_free(src);
        }
      }
      else result = EXIT_FAILURE;
    }
    fclose(file_src);
  }
  if (EXIT_SUCCESS == result && 0 != source_is_cl) {
    const char* ext_source = source;
    size_src = strlen(ext_source);
    if (NULL != extnames) {
      int n = num_exts, nflat = 0;
      size_t size_ext = 0;
      for (; 0 < n; --n) {
        if (NULL != extnames[n - 1]) {
          const char* const end = buffer + strlen(extnames[n - 1]); /* before strtok */
          char* ext = strtok(strncpy(buffer, extnames[n - 1], ACC_OPENCL_BUFFERSIZE - 1), ACC_OPENCL_DELIMS " \t");
          for (; NULL != ext; ext = ((ext + 1) < end ? strtok((ext + 1) + strlen(ext), ACC_OPENCL_DELIMS " \t") : NULL), ++nflat) {
            size_ext += strlen(ext);
          }
        }
      }
      if (0 < size_ext && 0 < nflat) {
        const char* const enable_ext = "#pragma OPENCL EXTENSION %s : enable\n";
        const size_t size_src_ext = size_src + size_ext + nflat * (strlen(enable_ext) - 2 /*%s*/);
        char* const ext_source_buffer = (char*)libxsmm_aligned_scratch(size_src_ext + 1 /*terminator*/, 0 /*auto-align*/);
        if (NULL != ext_source_buffer) {
          for (n = 0; 0 < num_exts; --num_exts) {
            if (NULL != extnames[num_exts - 1]) {
              const char* const end = buffer_name + strlen(extnames[num_exts - 1]); /* before strtok */
              char* ext = strtok(
                strncpy(buffer_name, extnames[num_exts - 1], ACC_OPENCL_MAXSTRLEN * 2 - 1), ACC_OPENCL_DELIMS " \t");
              for (; NULL != ext; ext = ((ext + 1) < end ? strtok((ext + 1) + strlen(ext), ACC_OPENCL_DELIMS " \t") : NULL)) {
                const char* line = source;
                for (;;) {
                  if (2 != sscanf(line, "#pragma OPENCL EXTENSION %[^: ]%*[: ]%[^\n]", buffer, buffer + ACC_OPENCL_BUFFERSIZE / 2))
                  {
                    line = NULL;
                    break;
                  }
                  else if (0 == strncmp(buffer, ext, ACC_OPENCL_BUFFERSIZE / 2) &&
                           0 == strncmp(buffer + ACC_OPENCL_BUFFERSIZE / 2, "enable", ACC_OPENCL_BUFFERSIZE / 2))
                  {
                    break;
                  }
                  line = strchr(line, '\n');
                  if (NULL != line) {
                    ++line;
                  }
                  else break;
                }
#  if !defined(NDEBUG)
                if (EXIT_SUCCESS == c_dbcsr_acc_opencl_device_ext(active_id, (const char* const*)&ext, 1))
#  endif
                { /* NDEBUG: assume given extension is supported (confirmed upfront) */
                  if (NULL == line) { /* extension is not already part of source */
                    n += LIBXSMM_SNPRINTF(ext_source_buffer + n, size_src_ext + 1 /*terminator*/ - n, enable_ext, ext);
                  }
                }
#  if !defined(NDEBUG)
                else if (0 != strcmp("cl_intel_global_float_atomics", ext)) {
                  fprintf(stderr, "WARN ACC/OpenCL: extension \"%s\" is not supported.\n", ext);
                }
#  endif
              }
            }
          }
          memcpy(ext_source_buffer + n, source, size_src);
          size_src += n; /* according to given/permitted extensions */
          assert(size_src <= size_src_ext);
          ext_source_buffer[size_src] = '\0';
          ext_source = ext_source_buffer;
        }
      }
      buffer[0] = '\0'; /* reset to empty */
    }
    /* consider preprocessing kernel for analysis (cpp); failure does not matter (result) */
#  if defined(ACC_OPENCL_CPPBIN)
    if (0 != c_dbcsr_acc_opencl_config.dump && NULL == file_src) {
      nchar = LIBXSMM_SNPRINTF(buffer_name, sizeof(buffer_name), ACC_OPENCL_TEMPDIR "/.%s.XXXXXX", kernel_name);
      if (0 < nchar && (int)sizeof(buffer_name) > nchar) {
        FILE* const file_cpp = fopen(ACC_OPENCL_CPPBIN, "rb");
        const char* sed_pattern = "";
#    if defined(ACC_OPENCL_SEDBIN)
        FILE* const file_sed = fopen(ACC_OPENCL_SEDBIN, "rb");
        if (NULL != file_sed) {
          sed_pattern = "| " ACC_OPENCL_SEDBIN " '/^[[:space:]]*\\(\\/\\/.*\\)*$/d'";
          fclose(file_sed); /* existence-check */
        }
#    endif
        if (NULL != file_cpp) {
          const int file_tmp = mkstemp(buffer_name);
          fclose(file_cpp); /* existence-check */
          if (0 <= file_tmp) {
            const int cl_std_len = (int)strlen(cl_std);
            nchar = LIBXSMM_SNPRINTF(buffer, sizeof(buffer),
              ACC_OPENCL_CPPBIN " -P -C -nostdinc -D__OPENCL_VERSION__=%u %s %s %s %s >%s.cl", 100 * level_major + 10 * level_minor,
              EXIT_SUCCESS != c_dbcsr_acc_opencl_device_vendor(active_id, "nvidia", 0 /*use_platform_name*/)
                ? ""
                : "-D__NV_CL_C_VERSION",
              NULL != build_params ? build_params : "", buffer_name, sed_pattern, kernel_name);
            if (0 < nchar && (int)sizeof(buffer) > nchar &&
                (0 == cl_std_len || (3 == write(file_tmp, "/*\n", 3) && cl_std_len == write(file_tmp, cl_std, cl_std_len) &&
                                      4 == write(file_tmp, "\n*/\n", 4))) &&
                size_src == (size_t)write(file_tmp, ext_source, size_src))
            {
              if (EXIT_SUCCESS == system(buffer)) {
                nchar = LIBXSMM_SNPRINTF(buffer, sizeof(buffer), "%s.cl", kernel_name);
                if (0 < nchar && (int)sizeof(buffer) > nchar) {
                  FILE* const file = fopen(buffer, "r");
                  if (NULL != file) {
                    const long int size = (EXIT_SUCCESS == fseek(file, 0 /*offset*/, SEEK_END) ? ftell(file) : 0);
                    char* const src = (char*)(EXIT_SUCCESS == fseek(file, 0 /*offset*/, SEEK_SET)
                                                ? libxsmm_aligned_scratch(size + 1 /*terminator*/, 0 /*auto-align*/)
                                                : NULL);
                    if (NULL != src) {
                      if ((size_t)size == fread(src, 1 /*sizeof(char)*/, size /*count*/, file)) {
                        if (source != ext_source) {
                          void* p = NULL;
                          LIBXSMM_ASSIGN127(&p, &ext_source);
                          libxsmm_free(p);
                        }
                        src[size] = '\0';
                        ext_source = src;
                      }
                      else libxsmm_free(src);
                    }
                    ACC_OPENCL_EXPECT(EXIT_SUCCESS == fclose(file));
                  }
                }
              }
            }
            buffer[0] = '\0'; /* reset to empty */
            ACC_OPENCL_EXPECT(EXIT_SUCCESS == unlink(buffer_name));
            ACC_OPENCL_EXPECT(EXIT_SUCCESS == close(file_tmp));
          }
        }
      }
    }
#  endif
    program = clCreateProgramWithSource(context, 1 /*nlines*/, &ext_source, NULL, &result);
    if (CL_SUCCESS == result) {
      assert(NULL != program);
      result = c_dbcsr_acc_opencl_flags(build_params, build_options, try_build_options, cl_std, buffer, sizeof(buffer));
      if (EXIT_SUCCESS == result) {
        result = clBuildProgram(program, 1 /*num_devices*/, &active_id, buffer, NULL /*callback*/, NULL /*user_data*/);
      }
      if (CL_SUCCESS != result && NULL != try_build_options && '\0' != *try_build_options) {
        result = c_dbcsr_acc_opencl_flags(build_params, build_options, NULL /*try_build_options*/, cl_std, buffer, sizeof(buffer));
        if (EXIT_SUCCESS == result) {
          ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program)); /* recreate below (to avoid unclean state) */
          program = clCreateProgramWithSource(context, 1 /*nlines*/, &ext_source, NULL, &result);
          assert(CL_SUCCESS != result || NULL != program);
          if (CL_SUCCESS == result) {
            result = clBuildProgram(program, 1 /*num_devices*/, &active_id, buffer, NULL /*callback*/, NULL /*user_data*/);
          }
        }
        ok = EXIT_FAILURE;
      }
      if (source != ext_source) {
        void* p = NULL;
        LIBXSMM_ASSIGN127(&p, &ext_source);
        libxsmm_free(p);
      }
      buffer[0] = '\0'; /* reset to empty */
      if (CL_SUCCESS == result) {
        *kernel = clCreateKernel(program, kernel_name, &result);
        if (CL_SUCCESS == result) {
          assert(NULL != *kernel);
          if (NULL == file_src && (2 <= c_dbcsr_acc_opencl_config.dump || 0 > c_dbcsr_acc_opencl_config.dump)) {
            unsigned char* binary = NULL;
            size_t size;
            binary = (unsigned char*)(CL_SUCCESS == clGetProgramInfo(program, CL_PROGRAM_BINARY_SIZES, sizeof(size_t), &size, NULL)
                                        ? libxsmm_aligned_scratch(size, 0 /*auto-align*/)
                                        : NULL);
            if (NULL != binary) {
              result = clGetProgramInfo(program, CL_PROGRAM_BINARIES, sizeof(unsigned char*), &binary, NULL);
              if (CL_SUCCESS == result) {
                FILE* file;
                nchar = LIBXSMM_SNPRINTF(buffer, sizeof(buffer), "%s.dump", kernel_name);
                file = (0 < nchar && (int)sizeof(buffer) > nchar) ? fopen(buffer, "wb") : NULL;
                buffer[0] = '\0'; /* reset to empty */
                if (NULL != file) {
                  if (size != fwrite(binary, 1, size, file)) {
                    ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
                    ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseKernel(*kernel));
                    result = EXIT_FAILURE;
                  }
                  fclose(file);
                }
                else {
                  ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
                  ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseKernel(*kernel));
                  result = EXIT_FAILURE;
                }
              }
              else { /* error: querying program binary */
                ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
                ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseKernel(*kernel));
              }
              libxsmm_free(binary);
            }
            else {
              ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
              ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseKernel(*kernel));
              result = EXIT_FAILURE;
            }
          }
        }
        else { /* error: creating kernel */
          ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
        }
      }
      else {
        ACC_OPENCL_EXPECT(
          CL_SUCCESS == clGetProgramBuildInfo(program, active_id, CL_PROGRAM_BUILD_LOG, ACC_OPENCL_BUFFERSIZE, buffer, NULL));
        ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
      }
    }
    else if (source != ext_source) { /* error: creating program */
      void* p = NULL;
      LIBXSMM_ASSIGN127(&p, &ext_source);
      libxsmm_free(p);
    }
  }
  else if (EXIT_SUCCESS == result) { /* binary representation */
#  if defined(CL_VERSION_2_1)
    if (0 != c_dbcsr_acc_opencl_config.dump) program = clCreateProgramWithIL(context, source, size_src, &result);
    else
#  endif
    {
      program = clCreateProgramWithBinary(
        context, 1, &active_id, &size_src, (const unsigned char**)&source, NULL /*binary_status*/, &result);
    }
    if (CL_SUCCESS == result) {
      assert(NULL != program);
      result = c_dbcsr_acc_opencl_flags(build_params, build_options, try_build_options, cl_std, buffer, sizeof(buffer));
      if (EXIT_SUCCESS == result) {
        result = clBuildProgram(program, 1 /*num_devices*/, &active_id, buffer, NULL /*callback*/, NULL /*user_data*/);
      }
      if (CL_SUCCESS != result && NULL != try_build_options && '\0' != *try_build_options) {
        result = c_dbcsr_acc_opencl_flags(build_params, build_options, NULL /*try_build_options*/, cl_std, buffer, sizeof(buffer));
        if (EXIT_SUCCESS == result) {
          ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program)); /* recreate below (to avoid unclean state) */
#  if defined(CL_VERSION_2_1)
          if (0 != c_dbcsr_acc_opencl_config.dump) program = clCreateProgramWithIL(context, source, size_src, &result);
          else
#  endif
          {
            program = clCreateProgramWithBinary(
              context, 1, &active_id, &size_src, (const unsigned char**)&source, NULL /*binary_status*/, &result);
          }
          assert(CL_SUCCESS != result || NULL != program);
          if (CL_SUCCESS == result) {
            result = clBuildProgram(program, 1 /*num_devices*/, &active_id, buffer, NULL /*callback*/, NULL /*user_data*/);
          }
        }
        ok = EXIT_FAILURE;
      }
      if (CL_SUCCESS == result) {
        *kernel = clCreateKernel(program, kernel_name, &result);
        assert(CL_SUCCESS != result || NULL != *kernel);
        if (CL_SUCCESS != result) { /* error: creating kernel */
#  if defined(CL_VERSION_1_2)
          /* discover available kernels in program, and adopt the last kernel listed */
          if (CL_SUCCESS == clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, sizeof(char*), buffer, NULL) && '\0' != *buffer) {
            const char *const semicolon = strrchr(buffer, ';'), *const name = (NULL == semicolon ? buffer : (semicolon + 1));
            *kernel = clCreateKernel(program, name, &result);
            assert(CL_SUCCESS != result || NULL != *kernel);
            if (CL_SUCCESS != result) ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
          }
          else
#  endif
          {
            ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
          }
        }
      }
      else {
        ACC_OPENCL_EXPECT(
          CL_SUCCESS == clGetProgramBuildInfo(program, active_id, CL_PROGRAM_BUILD_LOG, ACC_OPENCL_BUFFERSIZE, buffer, NULL));
        ACC_OPENCL_EXPECT(CL_SUCCESS == clReleaseProgram(program));
      }
    }
  }
  if (NULL != file_src) {
    void* p = NULL;
    LIBXSMM_ASSIGN127(&p, (const void**)&source);
    assert(0 != source_is_file);
    libxsmm_free(p);
  }
#  if !defined(NDEBUG)
  if (EXIT_SUCCESS != result && NULL != kernel) *kernel = NULL;
#  endif
  if (NULL != try_ok) *try_ok = result | ok;
  ACC_OPENCL_RETURN_CAUSE(result, buffer);
}

#  if defined(__cplusplus)
}
#  endif

#endif /*__OPENCL*/
