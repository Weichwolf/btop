// SPDX-License-Identifier: Apache-2.0
#include "linux/intel_gpu_memory.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {
using namespace Gpu::IntelMemory;
constexpr auto pci = "0000:00:02.0";

std::string record(std::string_view fields, unsigned int id = 1, std::string_view device = pci) {
	return "drm-driver: i915\ndrm-pdev: " + std::string(device) + "\ndrm-client-id: " + std::to_string(id) + "\n" +
		   std::string(fields);
}

std::optional<Client> parse(std::string_view text) {
	std::istringstream input{std::string(text)};
	return parse_fdinfo(input);
}

Client client(std::uint64_t allocated, std::uint64_t shared, std::uint64_t resident, unsigned int id = 1) {
	return Client{pci, id, {{"system0", Region{allocated, shared, resident}}}};
}

TEST(IntelGpuMemory, ParsesWhitespaceUnitsAndZeroValues) {
	const auto parsed =
		parse(record("drm-total-system0: 2 MiB\ndrm-shared-system0:\t0\ndrm-resident-system0: 3 KiB\n"));
	ASSERT_TRUE(parsed);
	EXPECT_EQ(parsed->regions.at("system0").allocated, 2 * 1024 * 1024);
	EXPECT_EQ(parsed->regions.at("system0").resident, 3 * 1024);
	EXPECT_EQ(parsed->regions.at("system0").shared, 0);
}

TEST(IntelGpuMemory, IgnoresEngineCycleCounters) {
	EXPECT_TRUE(parse(record(
		"drm-total-cycles-render: 123\ndrm-total-system0: 0\ndrm-shared-system0: 0\ndrm-resident-system0: 0\n")));
}

TEST(IntelGpuMemory, RejectsIncompleteDuplicateOrInconsistentRecords) {
	EXPECT_FALSE(parse(record("drm-total-system0: 10\ndrm-resident-system0: 5\n")));
	EXPECT_FALSE(parse(
		record("drm-total-system0: 10\ndrm-total-system0: 10\ndrm-shared-system0: 0\ndrm-resident-system0: 5\n")));
	EXPECT_FALSE(parse(record("drm-total-system0: 10\ndrm-shared-system0: 11\ndrm-resident-system0: 5\n")));
	EXPECT_FALSE(parse(record("drm-total-system0: 10\ndrm-shared-system0: 0\ndrm-resident-system0: 11\n")));
	EXPECT_FALSE(
		parse(record("drm-client-id: 2\ndrm-total-system0: 0\ndrm-shared-system0: 0\ndrm-resident-system0: 0\n")));
}

TEST(IntelGpuMemory, RejectsInvalidNumbersAndOverflow) {
	for (const auto value :
		 {"", "-1", "+1", "1.5 MiB", "1 GB", "9223372036854775808", "9007199254740992 KiB", "1 KiB garbage"}) {
		EXPECT_FALSE(parse(
			record("drm-total-system0: " + std::string(value) + "\ndrm-shared-system0: 0\ndrm-resident-system0: 0\n")))
			<< value;
	}
}

TEST(IntelGpuMemory, RejectsOversizedRecordsAndLines) {
	EXPECT_FALSE(parse(record(std::string(513, 'x'))));
	std::string large;
	for (int line = 0; line < 700; ++line)
		large += std::string(100, 'x') + '\n';
	EXPECT_FALSE(parse(record(large)));
}

TEST(IntelGpuMemory, CountsPrivateBuffersExactlyAndDeduplicatesClients) {
	auto first = client(100, 0, 60);
	auto duplicate = client(900, 0, 900);
	auto foreign = client(1000, 0, 1000, 2);
	foreign.pci = "0000:03:00.0";
	const auto snapshot = aggregate({first, duplicate, client(40, 0, 10, 2), foreign}, pci);
	ASSERT_TRUE(snapshot.available);
	EXPECT_EQ(snapshot.clients, 2);
	EXPECT_EQ(snapshot.allocated.lower, 140);
	EXPECT_EQ(snapshot.allocated.upper, 140);
	EXPECT_EQ(snapshot.resident.lower, 70);
	EXPECT_EQ(snapshot.resident.upper, 70);
}

TEST(IntelGpuMemory, BoundsPhysicalUnionOfSharedBuffers) {
	// Client 1: private 16 (8 resident), shared buffer 16 (resident).
	// Client 2: private 8 (resident), the SAME shared buffer 16.
	// Independent physical union: 40 allocated and 32 resident bytes.
	const auto snapshot = aggregate({client(32, 16, 24), client(24, 16, 24, 2)}, pci);
	ASSERT_TRUE(snapshot.available);
	EXPECT_EQ(snapshot.allocated.lower, 40);
	EXPECT_EQ(snapshot.allocated.upper, 56);
	EXPECT_EQ(snapshot.resident.lower, 32);
	EXPECT_EQ(snapshot.resident.upper, 48);
}

TEST(IntelGpuMemory, DoesNotMergeDistinctMemoryRegions) {
	auto first = client(16, 16, 16);
	auto second = client(32, 32, 32, 2);
	second.regions["stolen-system0"] = second.regions.extract("system0").mapped();
	const auto snapshot = aggregate({first, second}, pci);
	EXPECT_EQ(snapshot.allocated.lower, 48);
	EXPECT_EQ(snapshot.resident.lower, 48);
}

TEST(IntelGpuMemory, DistinguishesZeroFromUnavailableAndRejectsSumOverflow) {
	const auto zero = aggregate({client(0, 0, 0)}, pci);
	EXPECT_TRUE(zero.available);
	EXPECT_EQ(zero.resident.upper, 0);
	EXPECT_FALSE(aggregate({}, pci).available);
	const auto huge = static_cast<std::uint64_t>(std::numeric_limits<long long>::max());
	EXPECT_FALSE(aggregate({client(huge, 0, huge), client(1, 0, 1, 2)}, pci).available);
}

class IntelGpuMemoryProc : public testing::Test {
  protected:
	std::filesystem::path proc;

	void SetUp() override {
		std::string name = (std::filesystem::temp_directory_path() / "btop-gpu-test-XXXXXX").string();
		ASSERT_NE(::mkdtemp(name.data()), nullptr);
		proc = name;
	}
	void TearDown() override {
		std::filesystem::remove_all(proc);
	}
	void descriptor(int pid, int fd, std::string_view contents, std::string_view target = "/dev/dri/renderD128") {
		const auto process = proc / std::to_string(pid);
		std::filesystem::create_directories(process / "fd");
		std::filesystem::create_directories(process / "fdinfo");
		std::filesystem::create_symlink(target, process / "fd" / std::to_string(fd));
		std::ofstream(process / "fdinfo" / std::to_string(fd)) << contents;
	}
};

TEST_F(IntelGpuMemoryProc, DeduplicatesDescriptorsAcrossProcessesAndIgnoresNonDrmFiles) {
	const auto data = record("drm-total-system0: 16 KiB\ndrm-shared-system0: 0\ndrm-resident-system0: 8 KiB\n");
	descriptor(100, 3, data);
	descriptor(100, 4, data);
	descriptor(200, 5, data);
	descriptor(200, 6, data, "/tmp/not-a-drm-device");
	const auto snapshot = collect(proc, pci, getuid());
	ASSERT_TRUE(snapshot.available);
	EXPECT_EQ(snapshot.clients, 1);
	EXPECT_EQ(snapshot.allocated.lower, 16 * 1024);
	EXPECT_EQ(snapshot.resident.lower, 8 * 1024);
	EXPECT_FALSE(snapshot.partial);
}

TEST_F(IntelGpuMemoryProc, MarksMalformedDrmRecordsAsPartialAndFiltersUser) {
	descriptor(100, 3, "invalid");
	descriptor(200, 4, record("drm-total-system0: 4\ndrm-shared-system0: 0\ndrm-resident-system0: 2\n"));
	const auto snapshot = collect(proc, pci, getuid());
	EXPECT_TRUE(snapshot.available);
	EXPECT_TRUE(snapshot.partial);
	EXPECT_FALSE(collect(proc, pci, getuid() + 1).available);
	EXPECT_FALSE(collect(proc / "missing", pci, getuid()).available);
}
} // namespace
