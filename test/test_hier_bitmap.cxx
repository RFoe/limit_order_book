// HierBitmap against std::set, for several depths (1, 2 and 3 levels), and
// HugeBuffer basics.

#include <lob/mem/huge_buffer.hpp>
#include <lob/v3/hier_bitmap.hpp>
#include <lob/workload/generator.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <set>
#include <type_traits>
#include <vector>

namespace {

template <unsigned Bits> struct BitsTag : std::integral_constant<unsigned, Bits> {};

} // namespace

TEMPLATE_TEST_CASE("HierBitmap matches std::set", "[v3][bitmap]", BitsTag<6>, BitsTag<8>, BitsTag<12>,
                   BitsTag<13>, BitsTag<16>) {
    using Bitmap = lob::v3::HierBitmap<TestType::value>;
    std::vector<std::uint64_t> words(Bitmap::kTotalWords, 0);
    Bitmap                     bits(words.data());
    std::set<std::uint32_t>    ref;
    lob::workload::Rng         rng(TestType::value);

    REQUIRE(bits.empty());
    for (int step = 0; step < 200'000; ++step) {
        // cluster around a moving centre so words fill up and drain (summary bits flip)
        const auto centre = static_cast<std::uint32_t>(rng.uniform(Bitmap::kSize));
        const auto i      = static_cast<std::uint32_t>(
            (centre + rng.uniform(64)) % Bitmap::kSize);
        if (rng.bernoulli(0.5)) {
            bits.set(i);
            ref.insert(i);
        } else {
            bits.clear(i);
            ref.erase(i);
        }
        REQUIRE(bits.empty() == ref.empty());
        REQUIRE(bits.test(i) == ref.contains(i));
        if (!ref.empty()) {
            REQUIRE(bits.min() == *ref.begin());
            REQUIRE(bits.max() == *ref.rbegin());
        }
        if (step % 4096 == 0) REQUIRE(bits.consistent());
    }
    std::vector<std::uint32_t> seen;
    bits.for_each([&](std::uint32_t s) { seen.push_back(s); });
    CHECK(seen == std::vector<std::uint32_t>(ref.begin(), ref.end()));
    CHECK(bits.consistent());
}

TEST_CASE("HierBitmap depth", "[v3][bitmap]") {
    STATIC_CHECK(lob::v3::HierBitmap<6>::kDepth == 1);
    STATIC_CHECK(lob::v3::HierBitmap<12>::kDepth == 2);
    STATIC_CHECK(lob::v3::HierBitmap<16>::kDepth == 3);
    STATIC_CHECK(lob::v3::HierBitmap<16>::kTotalWords == 1024 + 16 + 1);
}

TEST_CASE("HugeBuffer is zeroed, writable and 2 MiB sized", "[v3][mem]") {
    lob::mem::HugeBuffer buf(3 << 20);
    REQUIRE(buf.data() != nullptr);
    CHECK(buf.size() == (4u << 20));
    const auto *bytes = static_cast<const unsigned char *>(buf.data());
    bool        zero  = true;
    for (std::size_t i = 0; i < buf.size(); i += 4096) zero = zero && bytes[i] == 0;
    CHECK(zero);
    std::memset(buf.data(), 0xab, buf.size());
    INFO("mode " << lob::mem::to_string(buf.mode()) << " huge_bytes " << buf.huge_bytes());
    if (buf.mode() != lob::mem::PageMode::Small) CHECK(buf.huge_bytes() > 0);
    lob::mem::HugeBuffer moved(std::move(buf));
    CHECK(buf.data() == nullptr);
    CHECK(moved.size() == (4u << 20));
}
