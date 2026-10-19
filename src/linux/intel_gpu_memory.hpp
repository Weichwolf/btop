// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Gpu::IntelMemory {
struct Region {
	std::optional<std::uint64_t> allocated;
	std::optional<std::uint64_t> shared;
	std::optional<std::uint64_t> resident;
};

struct Client {
	std::string pci;
	std::uint64_t id = 0;
	std::map<std::string, Region> regions;
};

struct Range {
	std::uint64_t lower = 0;
	std::uint64_t upper = 0;
};

struct Snapshot {
	Range allocated;
	Range resident;
	std::size_t clients = 0;
	bool available = false;
	bool partial = false;
};

std::optional<Client> parse_fdinfo(std::istream &input);
Snapshot aggregate(const std::vector<Client> &clients, std::string_view pci);
Snapshot collect(const std::filesystem::path &proc, std::string_view pci, unsigned int uid);
} // namespace Gpu::IntelMemory
