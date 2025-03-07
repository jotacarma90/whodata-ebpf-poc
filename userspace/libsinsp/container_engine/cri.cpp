// SPDX-License-Identifier: Apache-2.0
/*
Copyright (C) 2023 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <libsinsp/container_engine/cri.h>

#include <sys/stat.h>
#ifdef GRPC_INCLUDE_IS_GRPCPP
#include <grpcpp/grpcpp.h>
#else
#include <grpc++/grpc++.h>
#endif

#include <libsinsp/runc.h>
#include <libsinsp/container_engine/mesos.h>

#include <libsinsp/cri.hpp>

#include <memory>
#include <libsinsp/sinsp.h>
#include <libsinsp/sinsp_int.h>

using namespace std;
using namespace libsinsp::cri;
using namespace libsinsp::container_engine;
using namespace libsinsp::runc;

namespace {
// do the CRI communication asynchronously
bool s_async = true;

constexpr const cgroup_layout CRI_CGROUP_LAYOUT[] = {
        {"/", ""},                       // non-systemd containerd
        {"/crio-", ""},                  // non-systemd cri-o
        {"/cri-containerd-", ".scope"},  // systemd containerd
        {"/crio-", ".scope"},            // systemd cri-o
        {":cri-containerd:", ""},        // containerd without "SystemdCgroup = true"
        {"/docker-", ".scope"},          // systemd docker in cri-dockerd scenario
        {nullptr, nullptr}};
}  // namespace

cri::cri(container_cache_interface &cache, const std::string &cri_path, size_t engine_index):
        container_engine_base(cache) {
	m_engine_index = engine_index;
	auto unix_socket_path = scap_get_host_root() + cri_path;
	struct stat s = {};
	if(stat(unix_socket_path.c_str(), &s) != 0 || (s.st_mode & S_IFMT) != S_IFSOCK) {
		return;
	}

	m_cri_v1 = std::make_unique<libsinsp::cri::cri_interface_v1>(unix_socket_path);
	if(!m_cri_v1->is_ok()) {
		m_cri_v1.reset(nullptr);
	} else {
		return;
	}

	m_cri_v1alpha2 = std::make_unique<libsinsp::cri::cri_interface_v1alpha2>(unix_socket_path);
	if(!m_cri_v1alpha2->is_ok()) {
		m_cri_v1alpha2.reset(nullptr);
	}
}

void cri::cleanup() {
	if(m_async_source) {
		m_async_source->quiesce();
	}
	libsinsp::cri::cri_settings::set_cri_extra_queries(true);
}

void cri::set_cri_socket_path(const std::string &path) {
	libsinsp::cri::cri_settings::clear_cri_unix_socket_paths();
	add_cri_socket_path(path);
}

void cri::add_cri_socket_path(const std::string &path) {
	libsinsp::cri::cri_settings::add_cri_unix_socket_path(path);
}

void cri::set_cri_timeout(int64_t timeout_ms) {
	libsinsp::cri::cri_settings::set_cri_timeout(timeout_ms);
}

void cri::set_extra_queries(bool extra_queries) {
	libsinsp::cri::cri_settings::set_cri_extra_queries(extra_queries);
}

void cri::set_async(bool async) {
	s_async = async;
}

bool cri::resolve(sinsp_threadinfo *tinfo, bool query_os_for_missing_info) {
	container_cache_interface *cache = &container_cache();
	std::string container_id, cgroup;

	if(!matches_runc_cgroups(tinfo, CRI_CGROUP_LAYOUT, container_id, cgroup)) {
		return false;
	}
	tinfo->m_container_id = container_id;

	if(!m_cri_v1alpha2 && !m_cri_v1) {
		// This isn't an error in the case where the
		// configured unix domain socket doesn't exist. In
		// that case, s_cri isn't initialized at all. Hence,
		// the DEBUG.
		libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
		                          "cri (%s): Could not parse cri (no s_cri object)",
		                          container_id.c_str());
		return false;
	}

	if(!cache->should_lookup(container_id, get_cri_runtime_type(), m_engine_index)) {
		return true;
	}

	auto container = sinsp_container_info();
	container.m_id = container_id;
	container.m_type = get_cri_runtime_type();
	if(mesos::set_mesos_task_id(container, tinfo)) {
		libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
		                          "cri (%s) Mesos CRI container, Mesos task ID: [%s]",
		                          container_id.c_str(),
		                          container.m_mesos_task_id.c_str());
	}

	// note: query_os_for_missing_info is set to 'true' by default
	if(query_os_for_missing_info) {
		libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
		                          "cri (%s): Performing lookup",
		                          container_id.c_str());

		libsinsp::cgroup_limits::cgroup_limits_key key(container.m_id,
		                                               tinfo->get_cgroup("cpu"),
		                                               tinfo->get_cgroup("memory"),
		                                               tinfo->get_cgroup("cpuset"));

		if(!m_async_source) {
			// Each lookup attempt involves two CRI API calls (see
			// `cri_async_source::parse`), each one having a default timeout
			// of 1000ms (`cri::set_cri_timeout`).
			// On top of that, there's an exponential backoff with 125ms start
			// time (`sinsp_container_lookup::delay`) with a maximum of 5
			// retries.
			// The maximum time to complete all attempts can be then evaluated
			// with the following formula:
			//
			// max_wait_ms = (2 * s_cri_timeout) * n + (125 * (2^n - 1))
			//
			// Note that this excludes the time for the last 2 CRI API calls
			// that will be performed anyway, even if the TTL expires.
			//
			// With n=5 the result is 13875ms, we keep some margin as we are
			// taking into account elapsed time.
			uint64_t max_wait_ms = 20000;
			auto async_source =
			        new cri_async_source(cache, m_cri_v1alpha2.get(), m_cri_v1.get(), max_wait_ms);
			m_async_source = std::unique_ptr<cri_async_source>(async_source);
		}

		cache->set_lookup_status(container_id,
		                         get_cri_runtime_type(),
		                         sinsp_container_lookup::state::STARTED,
		                         m_engine_index);

		// sinsp_container_lookup is set-up to perform 5 retries at most, with
		// an exponential backoff with 2000 ms of maximum wait time.
		sinsp_container_info result(sinsp_container_lookup(5, 2000));

		bool done;
		const bool async = s_async && cache->async_allowed();
		if(async) {
			libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
			                          "cri_async (%s): Starting asynchronous lookup",
			                          container_id.c_str());
			done = m_async_source->lookup(key, result);
		} else {
			libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
			                          "cri_async (%s): Starting synchronous lookup",
			                          container_id.c_str());
			// `lookup_sync` function directly invokes the container engine specific parser `parse`
			done = m_async_source->lookup_sync(key, result);
			// note: The container image is the most crucial field from a security incident response
			// perspective. We aim to raise the bar for successful container lookups. Conversely,
			// pod sandboxes do not include a container image in the API response.
			if(!result.m_image.empty() || result.is_pod_sandbox()) {
				/*
				 * Only for synchronous lookup option (e.g. Falco's default is async not sync)
				 *
				 * Explicitly check for the most crucial retrieved value (`m_image`) to be present
				 * before enabling the fast-track container add option. At this point, the container
				 * with only the cgroup (container id) was already added to the cache. Therefore, we
				 * can proceed to call `replace_container`.
				 *
				 * Bypassing the round-trip process:
				 * `source_callback` -> `notify_new_container` ->
				 * `container_to_sinsp_event(container_to_json(container_info), ...)` ->
				 * `parse_container_json_evt` -> `m_inspector->m_container_manager.add_container()`
				 *
				 * In `parse_container_json_evt`, we still re-add the container to support native
				 * 'container' events and new container callbacks that may expect the container as
				 * JSON in the artificial sinsp evt. However, we can avoid delays by storing the
				 * container struct in the container cache now. This is beneficial because syscall
				 * events do not explicitly require container events, instead, they directly
				 * retrieve container details from the container cache. This new feature can
				 * mitigate issues noted by adopters, such as the absence of container images in
				 * syscall events even when disabling async lookups.
				 */
				result.set_lookup_status(sinsp_container_lookup::state::STARTED);
				// note: The cache should not have SUCCESSFUL as lookup status at this point, else
				// `parse_container_json_evt` would wrongly exit early.
				cache->replace_container(std::make_shared<sinsp_container_info>(result));
				// note: On the other hand `parse_container_json_evt` expects SUCCESSFUL as lookup
				// state for the incoming container event / the not yet cached container, exactly
				// how it was done within `lookup_sync`.
				result.set_lookup_status(sinsp_container_lookup::state::SUCCESSFUL);
			}
		}

		if(done) {
			// if a previous lookup call already found the metadata, process it now
			m_async_source->source_callback(key, result);

			if(async) {
				// This should *never* happen, in async mode as ttl is 0 (never wait)
				libsinsp_logger()->format(
				        sinsp_logger::SEV_ERROR,
				        "cri_async (%s): Unexpected immediate return from cri_async lookup",
				        container_id.c_str());
			}
		}
	} else {
		cache->notify_new_container(container, tinfo);
	}
	// note: with more than one container runtime we cannot be sure if a resolution is enough
	// and we have to query all the runtimes.
	return false;
}

void cri::update_with_size(const std::string &container_id) {
	sinsp_container_info::ptr_t existing = container_cache().get_container(container_id);
	if(!existing) {
		libsinsp_logger()->format(sinsp_logger::SEV_ERROR,
		                          "cri (%s): Failed to locate existing container data",
		                          container_id.c_str());
		ASSERT(false);
		return;
	}

	std::optional<int64_t> writable_layer_size = get_writable_layer_size(existing->m_full_id);

	if(!writable_layer_size.has_value()) {
		return;
	}

	// Make a mutable copy of the existing container_info
	shared_ptr<sinsp_container_info> updated(std::make_shared<sinsp_container_info>(*existing));
	updated->m_size_rw_bytes = *writable_layer_size;

	if(existing->m_size_rw_bytes == updated->m_size_rw_bytes) {
		// no data has changed
		return;
	}

	container_cache().replace_container(updated);
}

sinsp_container_type cri::get_cri_runtime_type() const {
	if(m_cri_v1) {
		return m_cri_v1->get_cri_runtime_type();
	} else if(m_cri_v1alpha2) {
		return m_cri_v1alpha2->get_cri_runtime_type();
	} else {
		return sinsp_container_type::CT_CRI;
	}
}

std::optional<int64_t> cri::get_writable_layer_size(const string &container_id) {
	if(m_cri_v1) {
		return m_cri_v1->get_writable_layer_size(container_id);
	} else if(m_cri_v1alpha2) {
		return m_cri_v1alpha2->get_writable_layer_size(container_id);
	} else {
		return std::nullopt;
	}
}

bool cri_async_source::parse(const cri_async_source::key_type &key,
                             sinsp_container_info &container) {
	if(m_cri_v1) {
		return m_cri_v1->parse(key, container);

	} else if(m_cri_v1alpha2) {
		return m_cri_v1alpha2->parse(key, container);
	}
	return false;
}
