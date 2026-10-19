// SPDX-License-Identifier: Apache-2.0
#include "intel_gpu_memory.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>

namespace Gpu::IntelMemory {
namespace {
constexpr std::uint64_t limit = std::numeric_limits<long long>::max();

std::string_view trim(std::string_view value) {
	const auto first = value.find_first_not_of(" \t\r");
	if (first == value.npos) return {};
	return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

std::optional<std::uint64_t> number(std::string_view text, bool bytes) {
	text = trim(text);
	if (text.empty()) return {};
	std::uint64_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{}) return {};
	const auto unit = trim(text.substr(static_cast<std::size_t>(end - text.data())));
	std::uint64_t factor = 1;
	if (bytes and unit == "KiB") factor = 1024;
	else if (bytes and unit == "MiB") factor = 1024 * 1024;
	else if (not unit.empty()) return {};
	if (value > limit / factor) return {};
	return value * factor;
}

bool numeric_name(std::string_view name) {
	return not name.empty() and
		   std::all_of(name.begin(), name.end(), [](char value) { return value >= '0' and value <= '9'; });
}

bool add(std::uint64_t &total, std::uint64_t value) {
	if (value > limit - total) return false;
	total += value;
	return true;
}

bool read_region(Client &client, std::string_view key, std::string_view value) {
	if (key.starts_with("drm-total-cycles-")) return true;
	std::optional<std::uint64_t> *destination = nullptr;
	if (key.starts_with("drm-total-")) destination = &client.regions[std::string(key.substr(10))].allocated;
	else if (key.starts_with("drm-shared-")) destination = &client.regions[std::string(key.substr(11))].shared;
	else if (key.starts_with("drm-resident-")) destination = &client.regions[std::string(key.substr(13))].resident;
	if (destination == nullptr) return true;
	if (destination->has_value()) return false;
	*destination = number(value, true);
	return destination->has_value();
}

struct RegionTotals {
	std::uint64_t allocated_private = 0;
	std::uint64_t allocated_shared_max = 0;
	std::uint64_t allocated_sum = 0;
	std::uint64_t resident_private_min = 0;
	std::uint64_t resident_shared_max = 0;
	std::uint64_t resident_sum = 0;
};

bool account(RegionTotals &total, const Region &region) {
	const auto allocated = *region.allocated;
	const auto shared = *region.shared;
	const auto resident = *region.resident;
	total.allocated_shared_max = std::max(total.allocated_shared_max, shared);
	total.resident_shared_max = std::max(total.resident_shared_max, std::min(resident, shared));
	return add(total.allocated_private, allocated - shared) and add(total.allocated_sum, allocated) and
		   add(total.resident_private_min, resident - std::min(resident, shared)) and add(total.resident_sum, resident);
}

bool disappeared(const std::error_code &error) {
	return error == std::errc::no_such_file_or_directory or error == std::errc::no_such_process;
}

void scan_process(const std::filesystem::path &process, std::vector<Client> &clients, bool &partial) {
	std::error_code error;
	std::filesystem::directory_iterator files(process / "fd", error), end;
	if (error and not disappeared(error)) partial = true;
	while (not error and files != end) {
		const auto descriptor = files->path();
		const auto target = std::filesystem::read_symlink(descriptor, error).string();
		if (error and not disappeared(error)) partial = true;
		if (not error and (target.starts_with("/dev/dri/card") or target.starts_with("/dev/dri/renderD"))) {
			std::ifstream input(process / "fdinfo" / descriptor.filename());
			if (input) {
				auto client = parse_fdinfo(input);
				if (client) clients.push_back(std::move(*client));
				else partial = true;
			} else {
				std::error_code exists_error;
				if (std::filesystem::exists(descriptor, exists_error) or
					(exists_error and not disappeared(exists_error)))
					partial = true;
			}
		}
		error.clear();
		files.increment(error);
		if (error and not disappeared(error)) partial = true;
	}
}
} // namespace

std::optional<Client> parse_fdinfo(std::istream &input) {
	Client client;
	std::optional<std::uint64_t> id;
	std::string driver;
	std::size_t size = 0;
	for (std::string line; std::getline(input, line);) {
		size += line.size() + 1;
		if (line.size() > 512 or size > 65536) return {};
		const auto colon = line.find(':');
		if (colon == line.npos) continue;
		const auto key = trim(std::string_view(line).substr(0, colon));
		const auto value = trim(std::string_view(line).substr(colon + 1));
		if (key == "drm-driver") {
			if (not driver.empty()) return {};
			driver = value;
		} else if (key == "drm-pdev") {
			if (not client.pci.empty()) return {};
			client.pci = value;
		} else if (key == "drm-client-id") {
			if (id) return {};
			id = number(value, false);
			if (not id) return {};
		} else if (not read_region(client, key, value)) return {};
	}
	if (input.bad() or driver != "i915" or client.pci.empty() or not id or client.regions.empty()) return {};
	for (const auto &[name, region] : client.regions) {
		if (name.empty() or not region.allocated or not region.shared or not region.resident) return {};
		if (*region.shared > *region.allocated or *region.resident > *region.allocated) return {};
	}
	client.id = *id;
	return client;
}

Snapshot aggregate(const std::vector<Client> &clients, std::string_view pci) {
	Snapshot result;
	std::map<std::string, RegionTotals> regions;
	std::set<std::uint64_t> seen;
	for (const auto &client : clients) {
		if (client.pci != pci or not seen.insert(client.id).second) continue;
		for (const auto &[name, region] : client.regions) {
			if (not region.allocated or not region.shared or not region.resident or
				*region.shared > *region.allocated or *region.resident > *region.allocated or
				not account(regions[name], region))
				return {};
		}
		result.clients++;
	}
	for (const auto &[name, total] : regions) {
		// Shared-buffer identities are absent from fdinfo. Report bounds on
		// their union instead of presenting a sum with duplicate buffers.
		if (not add(result.allocated.lower, total.allocated_private) or
			not add(result.allocated.lower, total.allocated_shared_max) or
			not add(result.allocated.upper, total.allocated_sum) or
			not add(result.resident.lower, total.resident_private_min) or
			not add(result.resident.lower, total.resident_shared_max) or
			not add(result.resident.upper, total.resident_sum))
			return {};
	}
	result.available = result.clients > 0;
	return result;
}

Snapshot collect(const std::filesystem::path &proc, std::string_view pci, unsigned int uid) {
	std::vector<Client> clients;
	bool partial = false;
	std::error_code error;
	std::filesystem::directory_iterator processes(proc, error), end;
	if (error) return {};
	while (not error and processes != end) {
		const auto process = processes->path();
		if (numeric_name(process.filename().string())) {
			struct stat status{};
			if (::stat(process.c_str(), &status) == 0 and status.st_uid == uid) scan_process(process, clients, partial);
		}
		processes.increment(error);
	}
	auto result = aggregate(clients, pci);
	result.partial = partial or static_cast<bool>(error);
	return result;
}
} // namespace Gpu::IntelMemory
