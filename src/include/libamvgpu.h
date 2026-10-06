/*
 * Copyright 2024 HAMi Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * AMD GPU (HIP) memory virtualization library for HAMi.
 * Intercepts HIP memory allocation calls via LD_AUDIT and enforces
 * per-device memory limits using shared memory IPC.
 */

#ifndef LIBAMVGPU_H
#define LIBAMVGPU_H

/* Environment variable names - matching HAMi convention */
#define HIP_DEVICE_MEMORY_LIMIT_ENV     "HIP_DEVICE_MEMORY_LIMIT"
#define HIP_DEVICE_MEMORY_LIMIT_ENV_FMT "HIP_DEVICE_MEMORY_LIMIT_%d"
#define HIP_DEVICE_MEMORY_SHARED_CACHE  "HIP_DEVICE_MEMORY_SHARED_CACHE"
#define ACTIVE_OOM_KILLER_ENV           "ACTIVE_OOM_KILLER"

/* Shared region path when HIP_DEVICE_MEMORY_SHARED_CACHE is unset. It is in
 * the container's own /tmp, so processes of one container share a limit. */
#define DEFAULT_SHARED_CACHE_PATH       "/tmp/hipdevshr.cache"

#endif /* LIBAMVGPU_H */
