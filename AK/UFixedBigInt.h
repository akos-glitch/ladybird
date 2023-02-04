/*
 * Copyright (c) 2021, Leon Albrecht <leon2002.la@gmail.com>
 * Copyright (c) 2023, Dan Klishch <danilklishch@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BigIntBase.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Checked.h>
#include <AK/Concepts.h>
#include <AK/Format.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtraDetails.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>

namespace AK {

namespace Detail {
// As noted near the declaration of StaticStorage, bit_size is more like a hint for a storage size.
// The effective bit size is `sizeof(StaticStorage<...>) * 8`. It is a programmer's responsibility
// to ensure that the hinted bit_size is always greater than the actual integer size.
// That said, do not use unaligned (bit_size % 64 != 0) `UFixedBigInt`s if you do not know what you
// are doing.
template<size_t bit_size, typename Storage = StaticStorage<false, bit_size>>
class UFixedBigInt;

// ===== Concepts =====
template<typename T>
constexpr inline size_t assumed_bit_size = 0;
template<>
constexpr inline size_t assumed_bit_size<IntegerWrapper> = bit_width<int>;
template<size_t bit_size>
constexpr inline size_t assumed_bit_size<UFixedBigInt<bit_size>> = bit_size;
template<BuiltInUFixedInt T>
constexpr inline size_t assumed_bit_size<T> = bit_width<T>;

template<typename T>
concept ConvertibleToUFixedInt = (assumed_bit_size<T> != 0);

template<typename T>
concept UFixedInt = (ConvertibleToUFixedInt<T> && !IsSame<T, IntegerWrapper>);

template<typename T>
concept NotBuiltInUFixedInt = (UFixedInt<T> && !BuiltInUFixedInt<T>);

// ===== UFixedBigInt itself =====
template<size_t bit_size>
constexpr auto& get_storage_of(UFixedBigInt<bit_size>& value) { return value.m_data; }

template<size_t bit_size>
constexpr auto& get_storage_of(UFixedBigInt<bit_size> const& value) { return value.m_data; }

template<typename Operand1, typename Operand2, typename Result>
constexpr void mul_internal(Operand1 const& operand1, Operand2 const& operand2, Result& result)
{
    StorageOperations::baseline_mul(operand1, operand2, result, g_null_allocator);
}

template<size_t bit_size, typename Storage>
class UFixedBigInt {
    constexpr static size_t static_size = Storage::static_size;
    constexpr static size_t part_size = static_size / 2;
    using UFixedBigIntPart = Conditional<part_size * word_size <= 64, u64, UFixedBigInt<part_size * word_size>>;

public:
    constexpr UFixedBigInt() = default;

    explicit constexpr UFixedBigInt(IntegerWrapper value) { StorageOperations::copy(value.m_data, m_data); }
    consteval UFixedBigInt(int value)
    {
        StorageOperations::copy(IntegerWrapper(value).m_data, m_data);
    }

    template<UFixedInt T>
    requires(sizeof(T) > sizeof(Storage)) explicit constexpr UFixedBigInt(T const& value)
    {
        StorageOperations::copy(get_storage_of(value), m_data);
    }

    template<UFixedInt T>
    requires(sizeof(T) <= sizeof(Storage)) constexpr UFixedBigInt(T const& value)
    {
        StorageOperations::copy(get_storage_of(value), m_data);
    }

    constexpr UFixedBigInt(UFixedBigIntPart const& low, UFixedBigIntPart const& high)
    requires(static_size % 2 == 0)
    {
        decltype(auto) low_storage = get_storage_of(low);
        decltype(auto) high_storage = get_storage_of(high);
        for (size_t i = 0; i < part_size; ++i)
            m_data[i] = low_storage[i];
        for (size_t i = 0; i < part_size; ++i)
            m_data[i + part_size] = high_storage[i];
    }

    template<UFixedInt T, size_t n>
    requires((assumed_bit_size<T> * n) <= bit_size) constexpr UFixedBigInt(const T (&value)[n])
    {
        size_t offset = 0;

        for (size_t i = 0; i < n; ++i) {
            if (offset % word_size == 0) {
                // Aligned initialization (i. e. u256 from two u128)
                decltype(auto) storage = get_storage_of(value[i]);
                for (size_t i = 0; i < storage.size(); ++i)
                    m_data[i + offset / word_size] = storage[i];
            } else if (offset % word_size == 32 && IsSame<T, u32>) {
                // u32 vector initialization on 64-bit platforms
                m_data[offset / word_size] |= static_cast<DoubleWord>(value[i]) << 32;
            } else {
                VERIFY_NOT_REACHED();
            }
            offset += assumed_bit_size<T>;
        }

        for (size_t i = (offset + word_size - 1) / word_size; i < m_data.size(); ++i)
            m_data[i] = 0;
    }

    // Casts & parts extraction
    template<NotBuiltInUFixedInt T>
    constexpr explicit operator T() const
    {
        T result;
        StorageOperations::copy(m_data, result.m_data);
        return result;
    }

    template<BuiltInUFixedInt T>
    requires(sizeof(T) <= sizeof(NativeWord)) constexpr explicit operator T() const
    {
        return m_data[0];
    }

    template<BuiltInUFixedInt T>
    requires(sizeof(T) == sizeof(DoubleWord)) constexpr explicit operator T() const
    {
        return (static_cast<DoubleWord>(m_data[1]) << word_size) + m_data[0];
    }

    constexpr UFixedBigIntPart low() const
    requires(static_size % 2 == 0)
    {
        if constexpr (part_size == 1) {
            return m_data[0];
        } else if constexpr (IsSame<UFixedBigIntPart, DoubleWord>) {
            return m_data[0] + (static_cast<DoubleWord>(m_data[1]) << word_size);
        } else {
            UFixedBigInt<part_size * word_size> result;
            StorageOperations::copy(m_data, result.m_data);
            return result;
        }
    }

    constexpr UFixedBigIntPart high() const
    requires(static_size % 2 == 0)
    {
        if constexpr (part_size == 1) {
            return m_data[part_size];
        } else if constexpr (IsSame<UFixedBigIntPart, DoubleWord>) {
            return m_data[part_size] + (static_cast<DoubleWord>(m_data[part_size + 1]) << word_size);
        } else {
            UFixedBigInt<part_size * word_size> result;
            StorageOperations::copy(m_data, result.m_data, part_size);
            return result;
        }
    }

    Bytes bytes()
    {
        return Bytes(reinterpret_cast<u8*>(this), sizeof(Storage));
    }

    ReadonlyBytes bytes() const
    {
        return ReadonlyBytes(reinterpret_cast<u8 const*>(this), sizeof(Storage));
    }

    constexpr UnsignedStorageSpan span()
    {
        return { m_data.data(), static_size };
    }

    constexpr UnsignedStorageReadonlySpan span() const
    {
        return { m_data.data(), static_size };
    }

    // Binary utils
    constexpr size_t popcnt() const
    {
        size_t result = 0;
        for (size_t i = 0; i < m_data.size(); ++i)
            result += popcount(m_data[i]);
        return result;
    }

    constexpr size_t ctz() const
    {
        size_t result = 0;
        for (size_t i = 0; i < m_data.size(); ++i) {
            if (m_data[i]) {
                result += count_trailing_zeroes(m_data[i]);
                break;
            } else {
                result += word_size;
            }
        }
        return result;
    }

    constexpr size_t clz() const
    {
        size_t result = 0;
        for (size_t i = m_data.size(); i--;) {
            if (m_data[i]) {
                result += count_leading_zeroes(m_data[i]);
                break;
            } else {
                result += word_size;
            }
        }
        return result + bit_size - word_size * static_size;
    }

    // Comparisons
    constexpr bool operator!() const
    {
        bool result = true;
        for (size_t i = 0; i < m_data.size(); ++i)
            result &= !m_data[i];
        return result;
    }

    constexpr explicit operator bool() const
    {
        bool result = false;
        for (size_t i = 0; i < m_data.size(); ++i)
            result |= m_data[i];
        return result;
    }

    constexpr bool operator==(UFixedInt auto const& other) const
    {
        return StorageOperations::compare(m_data, get_storage_of(other), true) == 0;
    }

    constexpr bool operator==(IntegerWrapper other) const
    {
        return StorageOperations::compare(m_data, get_storage_of(other), true) == 0;
    }

    constexpr int operator<=>(UFixedInt auto const& other) const
    {
        return StorageOperations::compare(m_data, get_storage_of(other), false);
    }

    constexpr int operator<=>(IntegerWrapper other) const
    {
        return StorageOperations::compare(m_data, get_storage_of(other), false);
    }

#define DEFINE_STANDARD_BINARY_OPERATOR(op, function)                        \
    constexpr auto operator op(UFixedInt auto const& other) const            \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        return do_standard_binary_operation(other, func);                    \
    }                                                                        \
                                                                             \
    constexpr auto operator op(IntegerWrapper other) const                   \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        return do_standard_binary_operation(other, func);                    \
    }

#define DEFINE_STANDARD_COMPOUND_ASSIGNMENT(op, function)                    \
    constexpr auto& operator op(UFixedInt auto const& other)                 \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        do_standard_compound_assignment(other, func);                        \
        return *this;                                                        \
    }                                                                        \
                                                                             \
    constexpr auto& operator op(IntegerWrapper other)                        \
    {                                                                        \
        auto func = [](auto&& a, auto&& b, auto&& c) { function(a, b, c); }; \
        do_standard_compound_assignment(other, func);                        \
        return *this;                                                        \
    }

    // Binary operators
    DEFINE_STANDARD_BINARY_OPERATOR(^, StorageOperations::compute_bitwise<StorageOperations::Bitwise::XOR>)
    DEFINE_STANDARD_BINARY_OPERATOR(&, StorageOperations::compute_bitwise<StorageOperations::Bitwise::AND>)
    DEFINE_STANDARD_BINARY_OPERATOR(|, StorageOperations::compute_bitwise<StorageOperations::Bitwise::OR>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(^=, StorageOperations::compute_inplace_bitwise<StorageOperations::Bitwise::XOR>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(&=, StorageOperations::compute_inplace_bitwise<StorageOperations::Bitwise::AND>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(|=, StorageOperations::compute_inplace_bitwise<StorageOperations::Bitwise::OR>)

    constexpr auto operator~() const
    {
        UFixedBigInt<bit_size> result;
        StorageOperations::compute_bitwise<StorageOperations::Bitwise::INVERT>(m_data, m_data, result.m_data);
        return result;
    }

    constexpr auto operator<<(size_t shift) const
    {
        UFixedBigInt<bit_size> result;
        StorageOperations::shift_left(m_data, shift, result.m_data);
        return result;
    }

    constexpr auto& operator<<=(size_t shift)
    {
        StorageOperations::shift_left(m_data, shift, m_data);
        return *this;
    }

    constexpr auto operator>>(size_t shift) const
    {
        UFixedBigInt<bit_size> result;
        StorageOperations::shift_right(m_data, shift, result.m_data);
        return result;
    }

    constexpr auto& operator>>=(size_t shift)
    {
        StorageOperations::shift_right(m_data, shift, m_data);
        return *this;
    }

    // Arithmetic
    template<UFixedInt T>
    constexpr auto addc(T const& other, bool& carry) const
    {
        UFixedBigInt<max(bit_size, assumed_bit_size<T>)> result;
        carry = StorageOperations::add<false>(m_data, get_storage_of(other), result.m_data, carry);
        return result;
    }

    template<UFixedInt T>
    constexpr auto subc(T const& other, bool& borrow) const
    {
        UFixedBigInt<max(bit_size, assumed_bit_size<T>)> result;
        borrow = StorageOperations::add<true>(m_data, get_storage_of(other), result.m_data, borrow);
        return result;
    }

    DEFINE_STANDARD_BINARY_OPERATOR(+, StorageOperations::add<false>)
    DEFINE_STANDARD_BINARY_OPERATOR(-, StorageOperations::add<true>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(+=, StorageOperations::add<false>)
    DEFINE_STANDARD_COMPOUND_ASSIGNMENT(-=, StorageOperations::add<true>)

    constexpr auto& operator++()
    {
        StorageOperations::increment<false>(m_data);
        return *this;
    }

    constexpr auto& operator--()
    {
        StorageOperations::increment<true>(m_data);
        return *this;
    }

    constexpr auto operator++(int)
    {
        UFixedBigInt<bit_size> result = *this;
        StorageOperations::increment<false>(m_data);
        return result;
    }

    constexpr auto operator--(int)
    {
        UFixedBigInt<bit_size> result = *this;
        StorageOperations::increment<true>(m_data);
        return result;
    }

    DEFINE_STANDARD_BINARY_OPERATOR(*, mul_internal)

    constexpr auto& operator*=(UFixedInt auto const& other) { return *this = *this * other; }
    constexpr auto& operator*=(IntegerWrapper const& other) { return *this = *this * other; }

    template<UFixedInt T>
    constexpr auto wide_multiply(T const& other) const
    {
        UFixedBigInt<bit_size + assumed_bit_size<T>> result;
        mul_internal(m_data, get_storage_of(other), result.m_data);
        return result;
    }

    // FIXME: Refactor out this
    using R = UFixedBigInt<bit_size>;

    static constexpr size_t my_size()
    {
        return sizeof(Storage);
    }

    // FIXME: Do something smarter (process at least one word per iteration).
    // FIXME: no restraints on this
    template<Unsigned U>
    requires(sizeof(Storage) >= sizeof(U)) constexpr R div_mod(U const& divisor, U& remainder) const
    {
        // FIXME: Is there a better way to raise a division by 0?
        //        Maybe as a compiletime warning?
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiv-by-zero"
        if (!divisor) {
            int volatile x = 1;
            int volatile y = 0;
            [[maybe_unused]] int volatile z = x / y;
        }
#pragma GCC diagnostic pop

        // fastpaths
        if (*this < divisor) {
            remainder = static_cast<U>(*this);
            return 0u;
        }
        if (*this == divisor) {
            remainder = 0u;
            return 1u;
        }
        if (divisor == 1u) {
            remainder = 0u;
            return *this;
        }

        remainder = 0u;
        R quotient = 0u;

        for (ssize_t i = sizeof(R) * 8 - clz() - 1; i >= 0; --i) {
            remainder <<= 1u;
            remainder |= static_cast<unsigned>(*this >> (size_t)i) & 1u;
            if (remainder >= divisor) {
                remainder -= divisor;
                quotient |= R { 1u } << (size_t)i;
            }
        }

        return quotient;
    }

    template<Unsigned U>
    constexpr R operator/(U const& other) const
    {
        U mod { 0u }; // unused
        return div_mod(other, mod);
    }
    template<Unsigned U>
    constexpr U operator%(U const& other) const
    {
        R res { 0u };
        div_mod(other, res);
        return res;
    }

    template<Unsigned U>
    constexpr R& operator/=(U const& other)
    {
        *this = *this / other;
        return *this;
    }
    template<Unsigned U>
    constexpr R& operator%=(U const& other)
    {
        *this = *this % other;
        return *this;
    }

    constexpr R sqrt() const
    {
        // Bitwise method: https://en.wikipedia.org/wiki/Integer_square_root#Using_bitwise_operations
        // the bitwise method seems to be way faster then Newtons:
        // https://quick-bench.com/q/eXZwW1DVhZxLE0llumeCXkfOK3Q
        if (*this == 1u)
            return 1u;

        ssize_t shift = (sizeof(R) * 8 - clz()) & ~1ULL;
        // should be equivalent to:
        // long shift = 2;
        // while ((val >> shift) != 0)
        //   shift += 2;

        R res = 0u;
        while (shift >= 0) {
            res = res << 1u;
            R large_cand = (res | 1u);
            if (*this >> (size_t)shift >= large_cand * large_cand)
                res = large_cand;
            shift -= 2;
        }
        return res;
    }

    constexpr R pow(u64 exp)
    {
        // Montgomery's Ladder Technique
        // https://en.wikipedia.org/wiki/Exponentiation_by_squaring#Montgomery's_ladder_technique
        R x1 = *this;
        R x2 = *this * *this;
        u64 exp_copy = exp;
        for (ssize_t i = sizeof(u64) * 8 - count_leading_zeroes(exp) - 2; i >= 0; --i) {
            if (exp_copy & 1u) {
                x2 *= x1;
                x1 *= x1;
            } else {
                x1 *= x2;
                x2 *= x2;
            }
            exp_copy >>= 1u;
        }
        return x1;
    }
    template<Unsigned U>
    requires(sizeof(U) > sizeof(u64)) constexpr R pow(U exp)
    {
        // Montgomery's Ladder Technique
        // https://en.wikipedia.org/wiki/Exponentiation_by_squaring#Montgomery's_ladder_technique
        R x1 = *this;
        R x2 = *this * *this;
        U exp_copy = exp;
        for (ssize_t i = sizeof(U) * 8 - exp().clz() - 2; i >= 0; --i) {
            if (exp_copy & 1u) {
                x2 *= x1;
                x1 *= x1;
            } else {
                x1 *= x2;
                x2 *= x2;
            }
            exp_copy >>= 1u;
        }
        return x1;
    }

    template<Unsigned U>
    constexpr U pow_mod(u64 exp, U mod)
    {
        // Left to right binary method:
        // https://en.wikipedia.org/wiki/Modular_exponentiation#Left-to-right_binary_method
        // FIXME: this is not sidechanel proof
        if (!mod)
            return 0u;

        U res = 1;
        u64 exp_copy = exp;
        for (size_t i = sizeof(u64) - count_leading_zeroes(exp) - 1u; i < exp; ++i) {
            res *= res;
            res %= mod;
            if (exp_copy & 1u) {
                res = (*this * res) % mod;
            }
            exp_copy >>= 1u;
        }
        return res;
    }
    template<Unsigned ExpT, Unsigned U>
    requires(sizeof(ExpT) > sizeof(u64)) constexpr U pow_mod(ExpT exp, U mod)
    {
        // Left to right binary method:
        // https://en.wikipedia.org/wiki/Modular_exponentiation#Left-to-right_binary_method
        // FIXME: this is not side channel proof
        if (!mod)
            return 0u;

        U res = 1;
        ExpT exp_copy = exp;
        for (size_t i = sizeof(ExpT) - exp.clz() - 1u; i < exp; ++i) {
            res *= res;
            res %= mod;
            if (exp_copy & 1u) {
                res = (*this * res) % mod;
            }
            exp_copy >>= 1u;
        }
        return res;
    }

    constexpr size_t log2()
    {
        // FIXME: proper rounding
        return sizeof(R) - clz();
    }
    constexpr size_t logn(u64 base)
    {
        // FIXME: proper rounding
        return log2() / (sizeof(u64) - count_leading_zeroes(base));
    }
    template<Unsigned U>
    requires(sizeof(U) > sizeof(u64)) constexpr size_t logn(U base)
    {
        // FIXME: proper rounding
        return log2() / base.log2();
    }

#undef DEFINE_STANDARD_BINARY_OPERATOR
#undef DEFINE_STANDARD_COMPOUND_ASSIGNMENT

    // These functions are intended to be used in LibCrypto for equality checks without branching.
    constexpr bool is_zero_constant_time() const
    {
        NativeWord fold = 0;
        for (size_t i = 0; i < m_data.size(); ++i)
            taint_for_optimizer(fold |= m_data[i]);
        return !fold;
    }

    constexpr bool is_equal_to_constant_time(UFixedBigInt<bit_size> other) const
    {
        NativeWord fold = 0;
        for (size_t i = 0; i < m_data.size(); ++i)
            taint_for_optimizer(fold |= m_data[i] ^ other.m_data[i]);
        return !fold;
    }

private:
    template<ConvertibleToUFixedInt T, typename Function>
    constexpr auto do_standard_binary_operation(T const& other, Function function) const
    {
        UFixedBigInt<max(bit_size, assumed_bit_size<T>)> result;
        function(m_data, get_storage_of(other), result.m_data);
        return result;
    }

    template<ConvertibleToUFixedInt T, typename Function>
    constexpr void do_standard_compound_assignment(T const& other, Function function)
    {
        static_assert(bit_size >= assumed_bit_size<T>, "Requested operation requires integer size to be expanded.");
        function(m_data, get_storage_of(other), m_data);
    }

    template<size_t other_bit_size, typename OtherStorage>
    friend class UFixedBigInt;

    friend constexpr auto& get_storage_of<bit_size>(UFixedBigInt<bit_size>&);
    friend constexpr auto& get_storage_of<bit_size>(UFixedBigInt<bit_size> const&);

    Storage m_data;
};

// FIXME: There is a bug in LLVM (https://github.com/llvm/llvm-project/issues/59783) which doesn't
//        allow to use the following comparisons.
bool operator==(BuiltInUFixedInt auto const& a, NotBuiltInUFixedInt auto const& b) { return b.operator==(a); }
int operator<=>(BuiltInUFixedInt auto const& a, NotBuiltInUFixedInt auto const& b) { return -b.operator<=>(a); }
bool operator==(IntegerWrapper const& a, NotBuiltInUFixedInt auto const& b) { return b.operator==(a); }
int operator<=>(IntegerWrapper const& a, NotBuiltInUFixedInt auto const& b) { return -b.operator<=>(a); }
}

using Detail::UFixedBigInt;

template<size_t bit_size>
constexpr inline bool IsUnsigned<UFixedBigInt<bit_size>> = true;
template<size_t bit_size>
constexpr inline bool IsSigned<UFixedBigInt<bit_size>> = false;

template<size_t bit_size>
struct NumericLimits<UFixedBigInt<bit_size>> {
    using T = UFixedBigInt<bit_size>;

    static constexpr T min() { return T {}; }
    static constexpr T max() { return --T {}; }
    static constexpr bool is_signed() { return false; }
};

// ===== Formatting =====
// FIXME: This does not work for size != 2 ** x
template<Detail::NotBuiltInUFixedInt T>
struct Formatter<T> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(formatter)
    {
    }

    ErrorOr<void> format(FormatBuilder& builder, T const& value)
    {
        using U = decltype(value.low());

        if (m_precision.has_value())
            VERIFY_NOT_REACHED();

        if (m_mode == Mode::Pointer) {
            // these are way to big for a pointer
            VERIFY_NOT_REACHED();
        }
        if (m_mode == Mode::Default)
            m_mode = Mode::Hexadecimal;

        if (!value.high()) {
            Formatter<U> formatter { *this };
            return formatter.format(builder, value.low());
        }

        u8 base = 0;
        if (m_mode == Mode::Binary) {
            base = 2;
        } else if (m_mode == Mode::BinaryUppercase) {
            base = 2;
        } else if (m_mode == Mode::Octal) {
            TODO();
        } else if (m_mode == Mode::Decimal) {
            TODO();
        } else if (m_mode == Mode::Hexadecimal) {
            base = 16;
        } else if (m_mode == Mode::HexadecimalUppercase) {
            base = 16;
        } else {
            VERIFY_NOT_REACHED();
        }
        ssize_t width = m_width.value_or(0);
        ssize_t lower_length = ceil_div(Detail::assumed_bit_size<U>, (ssize_t)base);
        Formatter<U> formatter { *this };
        formatter.m_width = max(width - lower_length, (ssize_t)0);
        TRY(formatter.format(builder, value.high()));
        TRY(builder.put_literal("'"sv));
        formatter.m_zero_pad = true;
        formatter.m_alternative_form = false;
        formatter.m_width = lower_length;
        TRY(formatter.format(builder, value.low()));
        return {};
    }
};
}

// these sizes should suffice for most usecases
using u128 = AK::UFixedBigInt<128>;
using u256 = AK::UFixedBigInt<256>;
using u512 = AK::UFixedBigInt<512>;
using u1024 = AK::UFixedBigInt<1024>;
using u2048 = AK::UFixedBigInt<2048>;
using u4096 = AK::UFixedBigInt<4096>;
