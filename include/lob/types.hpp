#pragma once
// Core vocabulary shared by every book version, the workload format and the
// ITCH converter. Changing anything here changes all versions at once.

#include <cstdint>
#include <type_traits>

namespace lob {

using Price   = std::int64_t;
using Qty     = std::uint32_t; // shares (ITCH share fields are uint32)
using OrderId = std::uint64_t; // integer ticks (ITCH: 1e-4 USD)

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr auto opposite(Side s) noexcept -> Side {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OpType : std::uint8_t { Push = 0, Cancel = 1, Modify = 2 };

// One input operation. This exact layout is what workload files store, so it is
// fixed-size, padding-free (pad is explicit and zeroed) and trivially copyable.
struct Op {
    OpType       type   = OpType::Push;
    Side         side   = Side::Buy; // Push only
    std::uint8_t pad[2] = {}; // always zero, keeps files byte-deterministic
    Qty          qty    = 0;  // Push / Modify
    OrderId      id     = 0;
    Price        price  = 0;  // Push / Modify

    friend auto operator==(const Op &, const Op &) -> bool = default;
};
static_assert(sizeof(Op) == 24);
static_assert(
    std::is_trivially_copyable_v<Op> && std::is_standard_layout_v<Op>);
static_assert(std::has_unique_object_representations_v<Op>);

constexpr auto make_push(OrderId id, Side side, Price price, Qty qty) noexcept
    -> Op {
    return Op{
        .type  = OpType::Push,
        .side  = side,
        .qty   = qty,
        .id    = id,
        .price = price};
}
constexpr auto make_cancel(OrderId id) noexcept -> Op {
    return Op{.type = OpType::Cancel, .id = id};
}
constexpr auto make_modify(OrderId id, Price price, Qty qty) noexcept -> Op {
    return Op{.type = OpType::Modify, .qty = qty, .id = id, .price = price};
}

} // namespace lob
