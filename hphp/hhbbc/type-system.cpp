/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#include "hphp/hhbbc/type-system.h"

#include <type_traits>
#include <cmath>
#include <algorithm>
#include <iterator>

#include <folly/Optional.h>
#include <folly/Traits.h>
#include <folly/Hash.h>

#include "hphp/runtime/base/array-init.h"
#include "hphp/runtime/base/array-iterator.h"
#include "hphp/runtime/base/double-to-int64.h"
#include "hphp/runtime/base/mixed-array.h"
#include "hphp/runtime/base/packed-array.h"
#include "hphp/runtime/base/repo-auth-type-array.h"
#include "hphp/runtime/base/repo-auth-type.h"
#include "hphp/runtime/base/set-array.h"
#include "hphp/runtime/base/tv-comparisons.h"

#include "hphp/hhbbc/eval-cell.h"
#include "hphp/hhbbc/index.h"

namespace HPHP { namespace HHBBC {

TRACE_SET_MOD(hhbbc);

#define X(y) const Type T##y = Type(B##y);
TYPES(X)
#undef X

namespace {

//////////////////////////////////////////////////////////////////////

const StaticString s_WaitHandle("HH\\WaitHandle");
const StaticString s_empty("");

//////////////////////////////////////////////////////////////////////

// When widening a type, allow no specialized information at a nesting depth
// greater than this. This keeps any such types from growing unbounded.
constexpr int kTypeWidenMaxDepth = 8;

//////////////////////////////////////////////////////////////////////

// Legal to call with !isPredefined(bits)
bool mayHaveData(trep bits) {
  switch (bits) {
  case BSStr:    case BObj:    case BInt:    case BDbl:
  case BOptSStr: case BOptObj: case BOptInt: case BOptDbl:
  case BCls:
  case BArr:     case BSArr:     case BCArr:
  case BArrN:    case BSArrN:    case BCArrN:
  case BOptArr:  case BOptSArr:  case BOptCArr:
  case BOptArrN: case BOptSArrN: case BOptCArrN:
  case BRef:
  case BVec:      case BSVec:      case BCVec:
  case BVecN:     case BSVecN:     case BCVecN:
  case BOptVec:   case BOptSVec:   case BOptCVec:
  case BOptVecN:  case BOptSVecN:  case BOptCVecN:
  case BDict:     case BSDict:     case BCDict:
  case BDictN:    case BSDictN:    case BCDictN:
  case BOptDict:  case BOptSDict:  case BOptCDict:
  case BOptDictN: case BOptSDictN: case BOptCDictN:
  case BKeyset:     case BSKeyset:     case BCKeyset:
  case BKeysetN:    case BSKeysetN:    case BCKeysetN:
  case BOptKeyset:  case BOptSKeyset:  case BOptCKeyset:
  case BOptKeysetN: case BOptSKeysetN: case BOptCKeysetN:
    return true;

  case BPArr:     case BSPArr:     case BCPArr:
  case BPArrN:    case BSPArrN:    case BCPArrN:
  case BOptPArr:  case BOptSPArr:  case BOptCPArr:
  case BOptPArrN: case BOptSPArrN: case BOptCPArrN:
  case BVArr:     case BSVArr:     case BCVArr:
  case BVArrN:    case BSVArrN:    case BCVArrN:
  case BOptVArr:  case BOptSVArr:  case BOptCVArr:
  case BOptVArrN: case BOptSVArrN: case BOptCVArrN:
  case BDArr:     case BSDArr:     case BCDArr:
  case BDArrN:    case BSDArrN:    case BCDArrN:
  case BOptDArr:  case BOptSDArr:  case BOptCDArr:
  case BOptDArrN: case BOptSDArrN: case BOptCDArrN:
    return true;

  case BBottom:
  case BUninit:
  case BInitNull:
  case BFalse:
  case BTrue:
  case BCStr:
  case BSArrE:
  case BCArrE:
  case BSVecE:
  case BCVecE:
  case BSDictE:
  case BCDictE:
  case BSKeysetE:
  case BCKeysetE:
  case BSPArrE:
  case BCPArrE:
  case BPArrE:
  case BSVArrE:
  case BCVArrE:
  case BVArrE:
  case BSDArrE:
  case BCDArrE:
  case BDArrE:
  case BRes:
  case BNull:
  case BNum:
  case BBool:
  case BStr:
  case BArrE:
  case BVecE:
  case BDictE:
  case BKeysetE:
  case BInitPrim:
  case BPrim:
  case BInitUnc:
  case BUnc:
  case BArrKey:
  case BUncArrKey:
  case BOptTrue:
  case BOptFalse:
  case BOptBool:
  case BOptNum:
  case BOptCStr:
  case BOptStr:
  case BOptSArrE:
  case BOptCArrE:
  case BOptArrE:
  case BOptSVecE:
  case BOptCVecE:
  case BOptVecE:
  case BOptSDictE:
  case BOptCDictE:
  case BOptDictE:
  case BOptSKeysetE:
  case BOptCKeysetE:
  case BOptKeysetE:
  case BOptSPArrE:
  case BOptCPArrE:
  case BOptPArrE:
  case BOptSVArrE:
  case BOptCVArrE:
  case BOptVArrE:
  case BOptSDArrE:
  case BOptCDArrE:
  case BOptDArrE:
  case BOptRes:
  case BOptArrKey:
  case BOptUncArrKey:
  case BInitCell:
  case BCell:
  case BInitGen:
  case BGen:
  case BTop:
    break;
  }
  return false;
}

/*
 * Note: currently we're limiting all represented types to predefined
 * bit patterns (instead of arbitrary unions), so this function is
 * around for assertions.
 *
 * This may be relaxed later if we think we can get more out of it,
 * but for now the thought is that the likelihood of getting
 * significantly better types from the inference algorithm might be
 * counter-balanced by the increased chance of hard-to-track
 * type-system bugs, so at least for now (at the time of this writing,
 * before shipping hhbbc) we're ruling out a bunch of edge cases.
 *
 * Aside from types like Obj<= or things like TSStr, a lot of cases
 * with arbitrary-unions may not lead to better code at JIT time
 * (unless the inference turns them back into a predefined type),
 * since we'll have to guard anyway to see which one it was.  We'll
 * try it later.
 */
bool isPredefined(trep bits) {
#define CASE(n) case B##n:
  switch (bits) {
    TYPES(CASE)
    return true;
    NON_TYPES(CASE)
    break;
  }
#undef CASE
  return false;
}

// Pre: isPredefined(bits)
bool canBeOptional(trep bits) {
  switch (bits) {
  case BBottom:
    return false;

  case BUninit:
  case BInitNull:
    return false;
  case BFalse:
  case BTrue:
  case BInt:
  case BDbl:
  case BSStr:
  case BSArrE:
  case BSArrN:
  case BSVecE:
  case BSVecN:
  case BSDictE:
  case BSDictN:
  case BSKeysetE:
  case BSKeysetN:
  case BObj:
  case BRes:
    return true;

  case BSPArrE:
  case BSPArrN:
  case BSPArr:
  case BPArrE:
  case BPArrN:
  case BPArr:
  case BSVArrE:
  case BSVArrN:
  case BSVArr:
  case BVArrE:
  case BVArrN:
  case BVArr:
  case BSDArrE:
  case BSDArrN:
  case BSDArr:
  case BDArrE:
  case BDArrN:
  case BDArr:
    return true;

  case BNull:
  case BNum:
  case BBool:
  case BStr:
  case BUncArrKey:
  case BArrKey:
  case BSArr:
  case BArrE:
  case BArrN:
  case BArr:
  case BSVec:
  case BVecE:
  case BVecN:
  case BVec:
  case BSDict:
  case BDictE:
  case BDictN:
  case BDict:
  case BSKeyset:
  case BKeysetE:
  case BKeysetN:
  case BKeyset:
    return true;

  case BCls:
  case BRef:
    return false;

  case BOptTrue:
  case BOptFalse:
  case BOptBool:
  case BOptInt:
  case BOptDbl:
  case BOptNum:
  case BOptSStr:
  case BOptStr:
  case BOptSArrE:
  case BOptSArrN:
  case BOptSArr:
  case BOptArrN:
  case BOptArrE:
  case BOptArr:
  case BOptSVecE:
  case BOptSVecN:
  case BOptSVec:
  case BOptVecN:
  case BOptVecE:
  case BOptVec:
  case BOptSDictE:
  case BOptSDictN:
  case BOptSDict:
  case BOptDictN:
  case BOptDictE:
  case BOptDict:
  case BOptSKeysetE:
  case BOptSKeysetN:
  case BOptSKeyset:
  case BOptKeysetN:
  case BOptKeysetE:
  case BOptKeyset:
  case BOptSPArrE:
  case BOptSPArrN:
  case BOptSPArr:
  case BOptPArrE:
  case BOptPArrN:
  case BOptPArr:
  case BOptSVArrE:
  case BOptSVArrN:
  case BOptSVArr:
  case BOptVArrE:
  case BOptVArrN:
  case BOptVArr:
  case BOptSDArrE:
  case BOptSDArrN:
  case BOptSDArr:
  case BOptDArrE:
  case BOptDArrN:
  case BOptDArr:
  case BOptObj:
  case BOptRes:
  case BOptUncArrKey:
  case BOptArrKey:
    return false;

  case BInitPrim:
  case BPrim:
  case BInitUnc:
  case BUnc:
  case BInitCell:
  case BInitGen:
  case BCell:
  case BGen:
  case BTop:
    return false;

#define CASE(n) case B##n:
    NON_TYPES(CASE)
    always_assert(false);
#undef CASE
  }
  not_reached();
}

/*
 * Combine array bits.  Our type system currently avoids arbitrary unions (see
 * rationale above), so we don't have predefined types like CArr|SArrN, or
 * SArrN|CArrE.  This function checks a few cases to ensure combining array
 * type bits leaves it predefined.
 */
template<trep B>
trep combine_arrish_bits(trep a, trep b) {
  DEBUG_ONLY constexpr trep OptB = static_cast<trep>(BInitNull | B);
  auto const combined = static_cast<trep>(a | b);
  assert((combined & OptB) == combined);
  auto const arr_part = static_cast<trep>(combined & B);
  if (!isPredefined(arr_part)) return static_cast<trep>(combined|B);
  assert(isPredefined(combined));
  return combined;
}

/*
 * Like combine_arrish_bits, but meant for combining bits representing two
 * different types with respect to d/varray-ness (IE, varray and darray). Takes
 * care of promoting to the right TArr union while maintaining the staticness
 * and emptiness bits.
 */
trep combine_dv_arrish_bits(trep a, trep b) {
  auto const combined = static_cast<trep>(a | b);
  auto const nonopt = trep(combined & ~BInitNull);
  auto const check = [&](trep x) { return (nonopt & x) == nonopt; };
  auto const ret = [&](trep x) {
    return (combined & BInitNull) ? trep(x | BInitNull) : x;
  };
  if (check(BSArrE)) return ret(BSArrE);
  if (check(BSArrN)) return ret(BSArrN);
  if (check(BSArr))  return ret(BSArr);
  if (check(BArrE))  return ret(BArrE);
  if (check(BArrN))  return ret(BArrN);
  if (check(BArr))   return ret(BArr);
  always_assert(false);
}

trep combine_arr_bits(trep a, trep b) {
  return combine_arrish_bits<BArr>(a, b);
}

trep combine_parr_bits(trep a, trep b) {
  return combine_arrish_bits<BPArr>(a, b);
}

trep combine_varr_bits(trep a, trep b) {
  return combine_arrish_bits<BVArr>(a, b);
}

trep combine_darr_bits(trep a, trep b) {
  return combine_arrish_bits<BDArr>(a, b);
}

trep combine_vec_bits(trep a, trep b) {
  return combine_arrish_bits<BVec>(a, b);
}

trep combine_dict_bits(trep a, trep b) {
  return combine_arrish_bits<BDict>(a, b);
}

trep combine_keyset_bits(trep a, trep b) {
  return combine_arrish_bits<BKeyset>(a, b);
}

// Combine bits; a must be a valid array-like trep; b should either be
// valid, or a repeated set such as (BSArrE | BSVecE | BSDictE |
// BSKeysetE). This lets you union in a particular set of the S,C,E
// and Opt bits without having to know which kind of array like
// structure you're dealing with (the bits that don't correspond to
// a's type will be dropped.
//
// Note that this allows you to combine bits representing different d/varray
// types. They'll promote to a TArr type.
trep combine_arr_like_bits(trep a, trep b) {
  auto check = [] (trep a, trep x) { return (a & x) == a; };
  assert(a && isPredefined(a) && !check(a, BInitNull));
  // If both bits have the same d/varray-ness, combine them as normal.
  if (check(a, BOptPArr) && check(b, BOptPArr)) return combine_parr_bits(a, b);
  if (check(a, BOptVArr) && check(b, BOptVArr)) return combine_varr_bits(a, b);
  if (check(a, BOptDArr) && check(b, BOptDArr)) return combine_darr_bits(a, b);
  // If they're all arrays, combine them and promote it to the right TArr union.
  if (check(a, BOptArr))    return combine_dv_arrish_bits(a, trep(b & BOptArr));
  if (check(a, BOptVec))    return combine_vec_bits(a,    trep(b & BOptVec));
  if (check(a, BOptDict))   return combine_dict_bits(a,   trep(b & BOptDict));
  if (check(a, BOptKeyset)) return combine_keyset_bits(a, trep(b & BOptKeyset));
  not_reached();
}

/*
 * Like combine_arr_like_bits, but treats d/varrays are completely separate
 * types from other arrays. Unlike combine_arr_like_bits, they'll never promote
 * to a TArr union. Useful for when you want to union in something like
 * TArrLikeN, but not promote a T[P,V,D]Arr to TArr.
 */
trep combine_dv_arr_like_bits(trep a, trep b) {
  auto check = [] (trep a, trep x) { return (a & x) == a; };
  assert(a && isPredefined(a) && !check(a, BInitNull));
  if (check(a, BOptPArr))   return combine_parr_bits(a,   trep(b & BOptPArr));
  if (check(a, BOptVArr))   return combine_varr_bits(a,   trep(b & BOptVArr));
  if (check(a, BOptDArr))   return combine_darr_bits(a,   trep(b & BOptDArr));
  if (check(a, BOptArr))    return combine_arr_bits(a,    trep(b & BOptArr));
  if (check(a, BOptVec))    return combine_vec_bits(a,    trep(b & BOptVec));
  if (check(a, BOptDict))   return combine_dict_bits(a,   trep(b & BOptDict));
  if (check(a, BOptKeyset)) return combine_keyset_bits(a, trep(b & BOptKeyset));
  not_reached();
}

trep maybe_promote_varray(trep a) {
  auto const check = [&](trep b, trep c){
    if (a & b) a = static_cast<trep>(a | c);
  };
  assert(isPredefined(a));
  check(BSVArrE, BSArrE);
  check(BCVArrE, BCArrE);
  check(BSVArrN, BSArrN);
  check(BCVArrN, BCArrN);
  assert(isPredefined(a));
  return a;
}

trep promote_varray(trep a) {
  auto const check = [&](trep b, trep c){
    if (a & b) a = static_cast<trep>((a | c) & ~b);
  };
  assert(isPredefined(a));
  // If the array is more than just a varray, we can't just switch the bits and
  // keep the combination predefined. Just use the maybe path which will keep
  // the bits predefined.
  if ((a & BOptVArr) != a) return maybe_promote_varray(a);
  check(BSVArrE, BSDArrE);
  check(BCVArrE, BCDArrE);
  check(BSVArrN, BSDArrN);
  check(BCVArrN, BCDArrN);
  assert(isPredefined(a));
  return a;
}

//////////////////////////////////////////////////////////////////////

template <typename T, typename... Args>
void construct(T& dest, Args&&... src) {
  new (&dest) T { std::forward<Args>(src)... };
}

template <typename T>
void destroy(T& t) { t.~T(); }

template <typename T, typename... Args>
void construct_inner(T& dest, Args&&... args) {
  construct(dest);
  dest.emplace(std::forward<Args>(args)...);
}

/*
 * The following functions make DArr* structs out of static arrays, to
 * simplify implementing some of the type system operations on them.
 *
 * When they return folly::none it is not a conservative thing: it
 * implies the array is definitely not packed, packedN, struct-like,
 * etc (we use this to return false in couldBe).
 */

folly::Optional<DArrLikePacked> toDArrLikePacked(SArray ar) {
  assert(!ar->empty());

  std::vector<Type> elems;
  auto idx = size_t{0};
  for (ArrayIter iter(ar); iter; ++iter, ++idx) {
    auto const key = *iter.first().asTypedValue();
    if (key.m_type != KindOfInt64) return folly::none;
    if (key.m_data.num != idx)     return folly::none;
    elems.push_back(
      from_cell(iter.secondVal())
    );
  }

  return DArrLikePacked { std::move(elems) };
}

folly::Optional<DArrLikePackedN> toDArrLikePackedN(SArray ar) {
  assert(!ar->empty());

  auto t = TBottom;
  auto idx = int64_t{0};
  for (ArrayIter iter(ar); iter; ++iter, ++idx) {
    auto const key = *iter.first().asTypedValue();
    if (key.m_type != KindOfInt64) return folly::none;
    if (key.m_data.num != idx)     return folly::none;
    t |= from_cell(iter.secondVal());
  }

  return DArrLikePackedN { std::move(t) };
}

folly::Optional<DArrLikeMap> toDArrLikeMap(SArray ar) {
  assert(!ar->empty());

  auto map = MapElems{};
  auto idx = int64_t{0};
  auto packed = true;
  for (ArrayIter iter(ar); iter; ++iter) {
    auto const key = *iter.first().asTypedValue();
    if (packed) {
      packed = isIntType(key.m_type) && key.m_data.num == idx;
      ++idx;
    }
    auto const value = iter.secondVal();
    map.emplace_back(key, from_cell(value));
  }
  if (packed) return folly::none;
  return DArrLikeMap { std::move(map) };
}

folly::Optional<DArrLikeMapN> toDArrLikeMapN(SArray ar) {
  assert(!ar->empty());

  auto k = TBottom;
  auto v = TBottom;
  auto idx = int64_t{0};
  auto packed = true;
  for (ArrayIter iter(ar); iter; ++iter) {
    auto const key = *iter.first().asTypedValue();
    k |= from_cell(key);
    v |= from_cell(iter.secondVal());
    if (packed) {
      packed = isIntType(key.m_type) && key.m_data.num == idx;
      ++idx;
    }
  }

  if (packed || tv(k)) return folly::none;
  return DArrLikeMapN { std::move(k), std::move(v) };
}

//////////////////////////////////////////////////////////////////////

bool subtypePacked(const DArrLikePacked& a, const DArrLikePacked& b) {
  auto const asz = a.elems.size();
  auto const bsz = b.elems.size();
  if (asz != bsz) return false;
  for (auto i = size_t{0}; i < asz; ++i) {
    if (!a.elems[i].subtypeOf(b.elems[i])) {
      return false;
    }
  }
  return true;
}

bool subtypeMap(const DArrLikeMap& a, const DArrLikeMap& b) {
  if (a.map.size() != b.map.size()) return false;
  auto aIt = begin(a.map);
  auto bIt = begin(b.map);
  for (; aIt != end(a.map); ++aIt, ++bIt) {
    if (!cellSame(aIt->first, bIt->first)) return false;
    if (!aIt->second.subtypeOf(bIt->second)) return false;
  }
  return true;
}

bool couldBePacked(const DArrLikePacked& a, const DArrLikePacked& b) {
  auto const asz = a.elems.size();
  auto const bsz = b.elems.size();
  if (asz != bsz) return false;
  for (auto i = size_t{0}; i < asz; ++i) {
    if (!a.elems[i].couldBe(b.elems[i])) {
      return false;
    }
  }
  return true;
}

bool couldBeMap(const DArrLikeMap& a, const DArrLikeMap& b) {
  if (a.map.size() != b.map.size()) return false;
  auto aIt = begin(a.map);
  auto bIt = begin(b.map);
  for (; aIt != end(a.map); ++aIt, ++bIt) {
    if (!cellSame(aIt->first, bIt->first)) return false;
    if (!aIt->second.couldBe(bIt->second)) return false;
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

std::pair<Type,Type> val_key_values(SArray a) {
  auto ret = std::make_pair(TBottom, TBottom);
  for (ArrayIter iter(a); iter; ++iter) {
    ret.first |= from_cell(*iter.first().asTypedValue());
    ret.second |= from_cell(*iter.secondRef().asTypedValue());
  }
  return ret;
}

std::pair<Type,Type> map_key_values(const DArrLikeMap& a) {
  auto ret = std::make_pair(TBottom, TBottom);
  for (auto const& kv : a.map) {
    ret.first |= from_cell(kv.first);
    ret.second |= kv.second;
  }
  return ret;
}

Type packed_values(const DArrLikePacked& a) {
  auto ret = TBottom;
  for (auto const& e : a.elems) ret |= e;
  return ret;
}

//////////////////////////////////////////////////////////////////////

/*
 * Helper for dealing with dualDispatchDataFn's---most of them are commutative.
 * This shuffles values to the right in a canonical order to need less
 * overloads.
 */
template<class InnerFn>
struct Commute : InnerFn {

  template<class... Args>
  explicit Commute(Args&&... args) : InnerFn(std::forward<Args>(args)...) {}

  using result_type = typename InnerFn::result_type;

  using InnerFn::operator();

  template<class B>
  typename std::enable_if<!std::is_same<SArray, B>::value, result_type>::type
  operator()(SArray a, const B& b) const {
    return InnerFn::operator()(b, a);
  }

  template<class B>
  result_type operator()(const DArrLikeMap& a, const B& b) const {
    return InnerFn::operator()(b, a);
  }

  template<class B>
  result_type operator()(const DArrLikePackedN& a, const B& b) const {
    return InnerFn::operator()(b, a);
  }

  template<class B>
  result_type operator()(const DArrLikeMapN& a, const B& b) const {
    return InnerFn::operator()(b, a);
  }
};

struct DualDispatchEqImpl {
  static constexpr bool disjoint = true;
  using result_type = bool;

  bool operator()() const { return false; }

  bool operator()(const DArrLikePacked& a, SArray b) const {
    if (a.elems.size() != b->size()) return false;
    auto const p = toDArrLikePacked(b);
    return p && a.elems == p->elems;
  }

  bool operator()(const DArrLikeMap& a, SArray b) const {
    if (a.map.size() != b->size()) return false;
    auto const m = toDArrLikeMap(b);
    return m && a.map == m->map;
  }

  bool operator()(const DArrLikePackedN& /*a*/, SArray /*b*/) const {
    return false;
  }
  bool operator()(const DArrLikeMapN& /*a*/, SArray /*b*/) const {
    return false;
  }
  bool
  operator()(const DArrLikePacked& /*a*/, const DArrLikePackedN& /*b*/) const {
    return false;
  }
  bool operator()(const DArrLikePacked& /*a*/, const DArrLikeMap& /*b*/) const {
    return false;
  }
  bool operator()(const DArrLikePacked&, const DArrLikeMapN&) const {
    return false;
  }
  bool
  operator()(const DArrLikePackedN& /*a*/, const DArrLikeMap& /*b*/) const {
    return false;
  }
  bool operator()(const DArrLikePackedN&, const DArrLikeMapN&) const {
    return false;
  }
  bool operator()(const DArrLikeMap&, const DArrLikeMapN&) const {
    return false;
  }
};

struct DualDispatchCouldBeImpl {
  static constexpr bool disjoint = true;
  using result_type = bool;

  bool operator()() const { return false; }

  bool operator()(const DArrLikePacked& a, SArray b) const {
    if (a.elems.size() != b->size()) return false;
    auto const p = toDArrLikePacked(b);
    return p && couldBePacked(a, *p);
  }

  bool operator()(const DArrLikeMap& a, SArray b) const {
    if (a.map.size() != b->size()) return false;
    auto const m = toDArrLikeMap(b);
    return m && couldBeMap(a, *m);
  }

  bool operator()(const DArrLikePackedN& a, SArray b) const {
    auto const p = toDArrLikePackedN(b);
    return p && a.type.couldBe(p->type);
  }

  bool operator()(const DArrLikeMapN& a, SArray b) const {
    assert(!b->empty());
    bool bad = false;
    IterateKV(
      b,
      [&] (Cell k, TypedValue v) {
        bad |= !(a.key.couldBe(from_cell(k)) && a.val.couldBe(from_cell(v)));
        return bad;
      }
    );
    return !bad;
  }

  bool operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    for (auto const& t : a.elems) {
      if (!t.couldBe(b.type)) return false;
    }
    return true;
  }

  bool operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    return TInt.couldBe(b.key) && a.type.couldBe(b.val);
  }

  bool operator()(const DArrLikeMap& a, const DArrLikeMapN& b) const {
    for (auto const& kv : a.map) {
      if (!from_cell(kv.first).couldBe(b.key)) return false;
      if (!kv.second.couldBe(b.val)) return false;
    }
    return true;
  }

  bool operator()(const DArrLikePacked& /*a*/, const DArrLikeMap& /*b*/) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    if (!TInt.couldBe(b.key)) return false;
    for (auto const& t : a.elems) {
      if (!t.couldBe(b.val)) return false;
    }
    return true;
  }
  bool
  operator()(const DArrLikePackedN& /*a*/, const DArrLikeMap& /*b*/) const {
    // Map does not contain any packed arrays.
    return false;
  }
};

// pre: neither type is a subtype of the other
struct DualDispatchIntersectionImpl {
  static constexpr bool disjoint = false;
  using result_type = Type;

  explicit DualDispatchIntersectionImpl(trep b) : bits(b) {}

  Type operator()() const { not_reached(); }

  template <typename F>
  Type intersect_packed(std::vector<Type> elems, F next) const {
    for (auto& e : elems) {
      e &= next();
      if (e == TBottom) return TBottom;
    }
    return packed_impl(bits, std::move(elems));
  }

  template <typename F>
  Type intersect_map(MapElems map, F next) const {
    for (auto it = map.begin(); it != map.end(); it++) {
      auto other = next();
      if (it->first.m_type == KindOfInt64 ?
          !other.first.couldBe(TInt) : !other.first.couldBe(TStr)) {
        return TBottom;
      }
      auto val = intersection_of(it->second, other.second);
      if (val == TBottom) return TBottom;
      map.update(it, std::move(val));
    }
    return map_impl(bits, std::move(map));
  }

  // The SArray is known to not be a subtype, so the intersection must be empty
  Type operator()(const DArrLikePacked& a, const SArray b) const {
    return TBottom;
  }
  Type operator()(const DArrLikePackedN& a, const SArray b) const {
    return TBottom;
  }
  Type operator()(const DArrLikeMapN& a, const SArray b) const {
    return TBottom;
  }
  Type operator()(const DArrLikeMap& a, const SArray b) const {
    return TBottom;
  }
  Type operator()(const SArray a, const SArray b) const {
    return TBottom;
  }

  Type operator()(const DArrLikePacked& a, const DArrLikePacked& b) const {
    if (a.elems.size() != b.elems.size()) return TBottom;

    auto i = size_t{};
    return intersect_packed(a.elems, [&] { return b.elems[i++]; });
  }
  Type operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    return intersect_packed(a.elems, [&] { return b.type; });
  }
  Type operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    if (b.key.couldBe(TInt)) {
      return intersect_packed(a.elems, [&] { return b.val; });
    }
    return TBottom;
  }
  Type operator()(const DArrLikePacked& a, const DArrLikeMap& b) const {
    // We don't allow DArrLikeMaps which are packed
    return TBottom;
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikePackedN& b) const {
    auto isect = intersection_of(a.type, b.type);
    if (isect == TBottom) return TBottom;
    return packedn_impl(bits, isect);
  }
  Type operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    if (b.key.couldBe(TInt)) {
      auto val = intersection_of(b.val, a.type);
      if (val != TBottom) return packedn_impl(bits, std::move(val));
    }
    return TBottom;
  }
  Type operator()(const DArrLikePackedN& a, const DArrLikeMap& b) const {
    return TBottom;
  }

  Type operator()(const DArrLikeMapN& a, const DArrLikeMapN& b) const {
    auto k = intersection_of(a.key, b.key);
    auto v = intersection_of(a.val, b.val);
    if (k == TBottom || v == TBottom) return TBottom;
    return mapn_impl(bits, k, v);
  }
  Type operator()(const DArrLikeMapN& a, const DArrLikeMap& b) const {
    return intersect_map(b.map, [&] { return std::make_pair(a.key, a.val); });
  }

  Type operator()(const DArrLikeMap& a, const DArrLikeMap& b) const {
    if (a.map.size() != b.map.size()) return TBottom;
    auto it = b.map.begin();
    return intersect_map(a.map, [&] {
      auto ret = std::make_pair(from_cell(it->first), it->second);
      ++it;
      return ret;
    });
  }
private:
  trep bits;
};

struct DualDispatchUnionImpl {
  static constexpr bool disjoint = false;
  using result_type = Type;

  explicit DualDispatchUnionImpl(trep b) : bits(b) {}

  Type operator()() const { not_reached(); }

  Type operator()(const DArrLikePacked& a, const DArrLikePacked& b) const {
    if (a.elems.size() != b.elems.size()) {
      return packedn_impl(bits, union_of(packed_values(a), packed_values(b)));
    }
    auto ret = a.elems;
    for (auto i = size_t{0}; i < a.elems.size(); ++i) {
      ret[i] |= b.elems[i];
    }
    return packed_impl(bits, std::move(ret));
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikePackedN& b) const {
    return packedn_impl(bits, union_of(a.type, b.type));
  }

  Type operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    return (*this)(DArrLikePackedN { packed_values(a) }, b);
  }

  Type operator()(const DArrLikeMap& a, const DArrLikeMap& b) const {
    auto to_map = [&] {
      auto mkva = map_key_values(a);
      auto mkvb = map_key_values(b);

      return mapn_impl(
        bits,
        union_of(std::move(mkva.first), std::move(mkvb.first)),
        union_of(std::move(mkva.second), std::move(mkvb.second)));
    };

    /*
     * With the current meaning of structs, if the keys are different, we can't
     * do anything better than going to a map type.  The reason for this is
     * that our struct types currently are implying the presence of all the
     * keys in the struct (it might be worth adding some more types for struct
     * subtyping to handle this better.)
     */
    if (a.map.size() != b.map.size()) return to_map();

    auto retStruct = MapElems{};
    auto aIt = begin(a.map);
    auto bIt = begin(b.map);
    for (; aIt != end(a.map); ++aIt, ++bIt) {
      if (!cellSame(aIt->first, bIt->first)) return to_map();
      retStruct.emplace_back(aIt->first, union_of(aIt->second, bIt->second));
    }
    return map_impl(bits, std::move(retStruct));
  }

  Type operator()(SArray a, SArray b) const {
    assert(a != b); // Should've been handled earlier in union_of.

    auto const p1 = toDArrLikePacked(a);
    auto const p2 = toDArrLikePacked(b);
    assert(!(bits & BVec) || (p1 && p2));

    if (p1) {
      if (p2) return (*this)(*p1, *p2);
      return (*this)(*p1, *toDArrLikeMap(b));
    } else if (p2) {
      return (*this)(*p2, *toDArrLikeMap(a));
    } else {
      return (*this)(*toDArrLikeMap(a), *toDArrLikeMap(b));
    }
  }

  Type operator()(const DArrLikeMapN& a, const DArrLikeMapN& b) const {
    return mapn_impl(bits, union_of(a.key, b.key), union_of(a.val, b.val));
  }

  Type operator()(const DArrLikePacked& a, SArray b) const {
    auto const p = toDArrLikePacked(b);
    if (p) return (*this)(a, *p);
    return (*this)(a, *toDArrLikeMap(b));
  }

  Type operator()(const DArrLikePackedN& a, SArray b) const {
    auto const p = toDArrLikePackedN(b);
    if (p) return (*this)(a, *p);
    return (*this)(a, *toDArrLikeMap(b));
  }

  Type operator()(const DArrLikeMap& a, SArray b) const {
    auto const m = toDArrLikeMap(b);
    if (m) return (*this)(a, *m);
    return (*this)(*toDArrLikePacked(b), a);
  }

  Type operator()(const DArrLikeMapN& a, SArray b) const {
    auto const m1 = toDArrLikeMapN(b);
    if (m1) return (*this)(a, *m1);
    auto const m2 = toDArrLikeMap(b);
    if (m2) return (*this)(*m2, a);
    return (*this)(*toDArrLikePackedN(b), a);
  }

  Type operator()(const DArrLikePacked& a, const DArrLikeMap& b) const {
    auto mkv = map_key_values(b);
    return mapn_impl(bits, union_of(TInt, std::move(mkv.first)),
                     union_of(packed_values(a), std::move(mkv.second)));
  }

  Type operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    return mapn_impl(bits, union_of(b.key, TInt),
                     union_of(packed_values(a), b.val));
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikeMap& b) const {
    auto mkv = map_key_values(b);
    return mapn_impl(bits, union_of(TInt, std::move(mkv.first)),
                     union_of(a.type, std::move(mkv.second)));
  }

  Type operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    return mapn_impl(bits, union_of(TInt, b.key), union_of(a.type, b.val));
  }

  Type operator()(const DArrLikeMap& a, const DArrLikeMapN& b) const {
    auto mkv = map_key_values(a);
    return mapn_impl(bits,
                     union_of(std::move(mkv.first), b.key),
                     union_of(std::move(mkv.second), b.val));
  }

private:
  trep bits;
};

/*
 * Subtype is not a commutative relation, so this is the only
 * dualDispatchDataFn helper that doesn't use Commute<>.
 */
struct DualDispatchSubtype {
  static constexpr bool disjoint = true;
  using result_type = bool;

  bool operator()() const { return false; }

  bool operator()(const DArrLikeMap& a, SArray b) const {
    if (a.map.size() != b->size()) return false;
    auto const m = toDArrLikeMap(b);
    return m && subtypeMap(a, *m);
  }

  bool operator()(SArray a, const DArrLikeMap& b) const {
    if (a->size() != b.map.size()) return false;
    auto const m = toDArrLikeMap(a);
    return m && subtypeMap(*m, b);
  }

  bool operator()(SArray a, const DArrLikePacked& b) const {
    if (a->size() != b.elems.size()) return false;
    auto const p = toDArrLikePacked(a);
    return p && subtypePacked(*p, b);
  }

  bool operator()(const DArrLikePacked& a, SArray b) const {
    if (a.elems.size() != b->size()) return false;
    auto const p = toDArrLikePacked(b);
    return p && subtypePacked(a, *p);
  }

  bool operator()(const DArrLikePackedN& a, const DArrLikeMapN& b) const {
    return b.key.couldBe(TInt) && a.type.subtypeOf(b.val);
  }

  bool operator()(const DArrLikePacked& a, const DArrLikeMapN& b) const {
    if (!b.key.couldBe(TInt)) return false;
    for (auto const& v : a.elems) {
      if (!v.subtypeOf(b.val)) return false;
    }
    return true;
  }

  bool operator()(const DArrLikeMap& a, const DArrLikeMapN& b) const {
    for (auto const& kv : a.map) {
      if (!from_cell(kv.first).subtypeOf(b.key)) return false;
      if (!kv.second.subtypeOf(b.val)) return false;
    }
    return true;
  }

  bool operator()(SArray a, const DArrLikeMapN& b) const {
    assert(!a->empty());
    bool bad = false;
    IterateKV(
      a,
      [&] (Cell k, TypedValue v) {
        bad |= !(b.key.couldBe(from_cell(k)) && b.val.couldBe(from_cell(v)));
        return bad;
      }
    );
    return !bad;
  }

  bool operator()(const DArrLikePacked& a, const DArrLikePackedN& b) const {
    for (auto const& t : a.elems) {
      if (!t.subtypeOf(b.type)) return false;
    }
    return true;
  }

  bool operator()(SArray a, const DArrLikePackedN& b) const {
    auto p = toDArrLikePackedN(a);
    return p && p->type.subtypeOf(b.type);
  }

  bool operator()(const DArrLikePackedN&, const DArrLikePacked&) const {
    // PackedN contains arrays with an arbitrary number of keys, while Packed
    // contains arrays with a fixed number of keys, so there's always arrays in
    // PackedN which aren't in Packed.
    return false;
  }
  bool operator()(const DArrLikePackedN&, SArray) const {
    // PackedN contains arrays with an arbitrary number of keys, while SArray is
    // just a single array.
    return false;
  }
  bool operator()(const DArrLikeMap& /*a*/, const DArrLikePacked& /*b*/) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool
  operator()(const DArrLikeMap& /*a*/, const DArrLikePackedN& /*b*/) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikePacked& /*a*/, const DArrLikeMap& /*b*/) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikePackedN&, const DArrLikeMap&) const {
    // Map does not contain any packed arrays.
    return false;
  }
  bool operator()(const DArrLikeMapN&, const DArrLikePackedN&) const {
    // MapN will always contain more arrays than PackedN because packed arrays
    // are a subset of all possible arrays.
    return false;
  }
  bool operator()(const DArrLikeMapN&, const DArrLikePacked&) const {
    // MapN contains arrays with an arbitrary number of keys, while Packed
    // contains arrays with a fixed number of keys, so there's always arrays in
    // MapN which aren't in Packed.
    return false;
  }
  bool operator()(const DArrLikeMapN&, const DArrLikeMap&) const {
    // MapN contains arrays with an arbitrary number of keys, while Map contains
    // arrays with a fixed number of keys, so there's always arrays in MapN
    // which aren't in Map.
    return false;
  }
  bool operator()(const DArrLikeMapN&, SArray) const {
    // MapN contains arrays with an arbitrary number of keys, while SArray is
    // just a single array.
    return false;
  }
};

using DualDispatchEq           = Commute<DualDispatchEqImpl>;
using DualDispatchCouldBe      = Commute<DualDispatchCouldBeImpl>;
using DualDispatchUnion        = Commute<DualDispatchUnionImpl>;
using DualDispatchIntersection = Commute<DualDispatchIntersectionImpl>;

//////////////////////////////////////////////////////////////////////
// Helpers for creating literal array-like types

template<typename AInit>
folly::Optional<Cell> fromTypeVec(const std::vector<Type> &elems) {
  AInit ai(elems.size());
  for (auto const& t : elems) {
    auto const v = tv(t);
    if (!v) return folly::none;
    ai.append(tvAsCVarRef(&*v));
  }
  auto var = ai.toVariant();
  var.setEvalScalar();
  return *var.asTypedValue();
}

bool checkTypeVec(const std::vector<Type> &elems) {
  for (auto const& t : elems) {
    if (!is_scalar(t)) return false;
  }
  return true;
}

Variant keyHelper(SString key) {
  return Variant{ key, Variant::PersistentStrInit{} };
}
const Variant& keyHelper(const Cell& v) {
  return tvAsCVarRef(&v);
}
template <typename AInit>
void add(AInit& ai, const Variant& key, const Variant& value) {
  ai.setValidKey(key, value);
}
void add(KeysetInit& ai, const Variant& key, const Variant& value) {
  assert(cellSame(*key.asTypedValue(), *value.asTypedValue()));
  ai.add(key);
}

template<typename AInit, typename Key>
folly::Optional<Cell> fromTypeMap(const ArrayLikeMap<Key> &elems) {
  auto val = eval_cell_value([&] () -> Cell {
    AInit ai(elems.size());
    for (auto const& elm : elems) {
      auto const v = tv(elm.second);
      if (!v) return make_tv<KindOfUninit>();
      add(ai, keyHelper(elm.first), tvAsCVarRef(&*v));
    }
    auto var = ai.toVariant();
    var.setEvalScalar();
    return *var.asTypedValue();
  });
  if (val && val->m_type == KindOfUninit) val.clear();
  return val;
}

template<typename Key>
bool checkTypeMap(const ArrayLikeMap<Key> &elems) {
  for (auto const& elm : elems) {
    if (!is_scalar(elm.second)) return false;
  }
  return true;
}

struct KeysetAppendInit : KeysetInit {
  using KeysetInit::KeysetInit;
  KeysetAppendInit& append(const Variant& v) {
    add(*v.asTypedValue());
    return *this;
  }
};

//////////////////////////////////////////////////////////////////////

}

//////////////////////////////////////////////////////////////////////

Type::Type(const Type& o) noexcept
  : m_bits(o.m_bits)
  , m_dataTag(o.m_dataTag)
{
  SCOPE_EXIT { assert(checkInvariants()); };
  switch (m_dataTag) {
    case DataTag::None:   return;
    #define DT(tag_name,type,name)              \
      case DataTag::tag_name:                   \
        construct(m_data.name, o.m_data.name);  \
        return;
    DATATAGS
    #undef DT
  }
  not_reached();
}

Type::Type(Type&& o) noexcept
  : m_bits(o.m_bits)
  , m_dataTag(o.m_dataTag)
{
  SCOPE_EXIT { assert(checkInvariants());
               assert(o.checkInvariants()); };
  o.m_dataTag = DataTag::None;
  switch (m_dataTag) {
    case DataTag::None:   return;
    #define DT(tag_name,type,name)                              \
      case DataTag::tag_name:                                   \
        construct(m_data.name, std::move(o.m_data.name));       \
        return;
    DATATAGS
    #undef DT
  }
  not_reached();
}

Type& Type::operator=(const Type& o) noexcept {
  SCOPE_EXIT { assert(checkInvariants()); };
  destroy(*this);
  construct(*this, o);
  return *this;
}

Type& Type::operator=(Type&& o) noexcept {
  SCOPE_EXIT { assert(checkInvariants());
               assert(o.checkInvariants()); };
  destroy(*this);
  construct(*this, std::move(o));
  return *this;
}

Type::~Type() noexcept {
  assert(checkInvariants());

  switch (m_dataTag) {
    case DataTag::None: return;
    #define DT(tag_name,type,name)              \
      case DataTag::tag_name:                   \
        destroy(m_data.name);                   \
        return;
    DATATAGS
    #undef DT
  }
  not_reached();
}

const Type& Type::operator |= (const Type& other) {
  *this = union_of(std::move(*this), other);
  return *this;
}

const Type& Type::operator |= (Type&& other) {
  *this = union_of(std::move(*this), std::move(other));
  return *this;
}

const Type& Type::operator &= (const Type& other) {
  *this = intersection_of(std::move(*this), other);
  return *this;
}

const Type& Type::operator &= (Type&& other) {
  *this = intersection_of(std::move(*this), std::move(other));
  return *this;
}

//////////////////////////////////////////////////////////////////////

template<class Ret, class T, class Function>
struct Type::DDHelperFn {
  template<class Y>
  typename std::enable_if<!std::is_same<Y,T>::value ||
                          !Function::disjoint, Ret>::type
  operator()(const Y& y) const { return f(t, y); }

  template <class Y>
  typename std::enable_if<std::is_same<Y, T>::value && Function::disjoint,
                          Ret>::type
  operator()(const Y& /*y*/) const {
    not_reached();
  }
  Ret operator()() const { return f(); }
  Function f;
  const T& t;
};

template<class Ret, class T, class Function>
Type::DDHelperFn<Ret,T,Function> Type::ddbind(const Function& f,
                                              const T& t) const {
  return { f, t };
}

// Dispatcher for the second argument for dualDispatchDataFn.
template<class Ret, class T, class Function>
Ret Type::dd2nd(const Type& o, DDHelperFn<Ret,T,Function> f) const {
  switch (o.m_dataTag) {
  case DataTag::None:           not_reached();
  case DataTag::Str:            return f();
  case DataTag::Obj:            return f();
  case DataTag::Int:            return f();
  case DataTag::Dbl:            return f();
  case DataTag::Cls:            return f();
  case DataTag::RefInner:       return f();
  case DataTag::ArrLikeVal:     return f(o.m_data.aval);
  case DataTag::ArrLikePacked:  return f(*o.m_data.packed);
  case DataTag::ArrLikePackedN: return f(*o.m_data.packedn);
  case DataTag::ArrLikeMap:     return f(*o.m_data.map);
  case DataTag::ArrLikeMapN:    return f(*o.m_data.mapn);
  }
  not_reached();
}

/*
 * Dual-dispatch on (this->m_dataTag, o.m_dataTag)
 *
 * See the DualDispatch* classes above
 */
template<class Function>
typename Function::result_type
Type::dualDispatchDataFn(const Type& o, Function f) const {
  using R = typename Function::result_type;
  switch (m_dataTag) {
  case DataTag::None:           not_reached();
  case DataTag::Str:            return f();
  case DataTag::Obj:            return f();
  case DataTag::Int:            return f();
  case DataTag::Dbl:            return f();
  case DataTag::Cls:            return f();
  case DataTag::RefInner:       return f();
  case DataTag::ArrLikeVal:     return dd2nd(o, ddbind<R>(f, m_data.aval));
  case DataTag::ArrLikePacked:  return dd2nd(o, ddbind<R>(f, *m_data.packed));
  case DataTag::ArrLikePackedN: return dd2nd(o, ddbind<R>(f, *m_data.packedn));
  case DataTag::ArrLikeMap:     return dd2nd(o, ddbind<R>(f, *m_data.map));
  case DataTag::ArrLikeMapN:    return dd2nd(o, ddbind<R>(f, *m_data.mapn));
  }
  not_reached();
}

//////////////////////////////////////////////////////////////////////

bool Type::hasData() const {
  return m_dataTag != DataTag::None;
}

bool Type::equivData(const Type& o) const {
  if (m_dataTag != o.m_dataTag) {
    return dualDispatchDataFn(o, DualDispatchEq{});
  }

  switch (m_dataTag) {
  case DataTag::None:
    not_reached();
  case DataTag::Str:
    return m_data.sval == o.m_data.sval;
  case DataTag::ArrLikeVal:
    return m_data.aval == o.m_data.aval;
  case DataTag::Int:
    return m_data.ival == o.m_data.ival;
  case DataTag::Dbl:
    // +ve and -ve zero must not compare equal, but (for purposes of
    // Type equivalence), NaNs are equal.
    return m_data.dval == o.m_data.dval ?
      std::signbit(m_data.dval) == std::signbit(o.m_data.dval) :
      (std::isnan(m_data.dval) && std::isnan(o.m_data.dval));
  case DataTag::Obj:
    if (!m_data.dobj.whType != !o.m_data.dobj.whType) return false;
    if (m_data.dobj.whType && *m_data.dobj.whType != *o.m_data.dobj.whType) {
      return false;
    }
    return m_data.dobj.type == o.m_data.dobj.type &&
           m_data.dobj.cls.same(o.m_data.dobj.cls);
  case DataTag::Cls:
    return m_data.dcls.type == o.m_data.dcls.type &&
           m_data.dcls.cls.same(o.m_data.dcls.cls);
  case DataTag::RefInner:
    return *m_data.inner == *o.m_data.inner;
  case DataTag::ArrLikePacked:
    return m_data.packed->elems == o.m_data.packed->elems;
  case DataTag::ArrLikePackedN:
    return m_data.packedn->type == o.m_data.packedn->type;
  case DataTag::ArrLikeMap:
    return m_data.map->map == o.m_data.map->map;
  case DataTag::ArrLikeMapN:
    return m_data.mapn->key == o.m_data.mapn->key &&
           m_data.mapn->val == o.m_data.mapn->val;
  }
  not_reached();
}

bool Type::subtypeData(const Type& o) const {
  if (m_dataTag != o.m_dataTag) {
    return dualDispatchDataFn(o, DualDispatchSubtype{});
  }

  switch (m_dataTag) {
  case DataTag::Obj:
  {
    auto const outer_ok = [&] {
      if (m_data.dobj.type == o.m_data.dobj.type &&
          m_data.dobj.cls.same(o.m_data.dobj.cls)) {
        return true;
      }
      if (o.m_data.dobj.type == DObj::Sub) {
        return m_data.dobj.cls.subtypeOf(o.m_data.dobj.cls);
      }
      return false;
    }();
    if (!outer_ok) return false;
    if (!o.m_data.dobj.whType) return true;
    if (!m_data.dobj.whType) return false;
    return m_data.dobj.whType->subtypeOf(*o.m_data.dobj.whType);
  }
  case DataTag::Cls:
    if (m_data.dcls.type == o.m_data.dcls.type &&
        m_data.dcls.cls.same(o.m_data.dcls.cls)) {
      return true;
    }
    if (o.m_data.dcls.type == DCls::Sub) {
      return m_data.dcls.cls.subtypeOf(o.m_data.dcls.cls);
    }
    return false;
  case DataTag::Str:
  case DataTag::ArrLikeVal:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::None:
    return equivData(o);
  case DataTag::RefInner:
    return m_data.inner->subtypeOf(*o.m_data.inner);
  case DataTag::ArrLikePacked:
    return subtypePacked(*m_data.packed, *o.m_data.packed);
  case DataTag::ArrLikePackedN:
    return m_data.packedn->type.subtypeOf(o.m_data.packedn->type);
  case DataTag::ArrLikeMap:
    return subtypeMap(*m_data.map, *o.m_data.map);
  case DataTag::ArrLikeMapN:
    return m_data.mapn->key.subtypeOf(o.m_data.mapn->key) &&
           m_data.mapn->val.subtypeOf(o.m_data.mapn->val);
  }
  not_reached();
}

bool Type::couldBeData(const Type& o) const {
  if (m_dataTag != o.m_dataTag) {
    return dualDispatchDataFn(o, DualDispatchCouldBe{});
  }

  switch (m_dataTag) {
  case DataTag::None:
    not_reached();
  case DataTag::Obj:
  {
    auto couldBe = [&] {
      if (m_data.dobj.type == o.m_data.dobj.type &&
          m_data.dobj.cls.same(o.m_data.dobj.cls)) {
        return true;
      }
      if (m_data.dobj.type == DObj::Sub) {
        if (o.m_data.dobj.type == DObj::Sub) {
          return o.m_data.dobj.cls.couldBe(m_data.dobj.cls);
        }
        return o.m_data.dobj.cls.subtypeOf(m_data.dobj.cls);
      }
      if (o.m_data.dobj.type == DObj::Sub) {
        return m_data.dobj.cls.subtypeOf(o.m_data.dobj.cls);
      }
      return false;
    }();
    return couldBe && (!o.m_data.dobj.whType ||
                       !m_data.dobj.whType ||
                       m_data.dobj.whType->couldBe(*o.m_data.dobj.whType));
  }
  case DataTag::Cls:
    if (m_data.dcls.type == o.m_data.dcls.type &&
        m_data.dcls.cls.same(o.m_data.dcls.cls)) {
      return true;
    }
    if (m_data.dcls.type == DCls::Sub || o.m_data.dcls.type == DCls::Sub) {
      return m_data.dcls.cls.couldBe(o.m_data.dcls.cls);
    }
    return false;
  case DataTag::RefInner:
    return m_data.inner->couldBe(*o.m_data.inner);
  case DataTag::Str:
  case DataTag::ArrLikeVal:
  case DataTag::Int:
  case DataTag::Dbl:
    return equivData(o);
  case DataTag::ArrLikePacked:
    return couldBePacked(*m_data.packed, *o.m_data.packed);
  case DataTag::ArrLikePackedN:
    return m_data.packedn->type.couldBe(o.m_data.packedn->type);
  case DataTag::ArrLikeMap:
    return couldBeMap(*m_data.map, *o.m_data.map);
  case DataTag::ArrLikeMapN:
    return m_data.mapn->key.couldBe(o.m_data.mapn->key) &&
           m_data.mapn->val.couldBe(o.m_data.mapn->val);
  }
  not_reached();
}

bool Type::operator==(const Type& o) const {
  // NB: We don't assert checkInvariants() here because this can be called from
  // checkInvariants() and it all takes too long if the type is deeply nested.

  if (m_bits != o.m_bits) return false;
  if (hasData() != o.hasData()) return false;
  if (!hasData()) return true;

  return equivData(o);
}

size_t Type::hash() const {
  using U1 = std::underlying_type<decltype(m_bits)>::type;
  using U2 = std::underlying_type<decltype(m_dataTag)>::type;
  auto const rawBits = U1{m_bits};
  auto const rawTag  = static_cast<U2>(m_dataTag);
  return folly::hash::hash_combine(rawBits, rawTag);
}

bool Type::subtypeOf(const Type& o) const {
  // NB: We don't assert checkInvariants() here because this can be called from
  // checkInvariants() and it all takes too long if the type is deeply nested.

  auto const isect = static_cast<trep>(m_bits & o.m_bits);
  if (isect != m_bits) return false;

  // No data is always more general.
  if (!o.hasData()) return true;
  if (!hasData()) return !mayHaveData(m_bits);

  // Both have data, so it depends on what the data says.
  return subtypeData(o);
}

bool Type::strictSubtypeOf(const Type& o) const {
  assert(checkInvariants());
  assert(o.checkInvariants());
  return *this != o && subtypeOf(o);
}

bool Type::couldBe(const Type& o) const {
  assert(checkInvariants());
  assert(o.checkInvariants());

  auto const isect = static_cast<trep>(m_bits & o.m_bits);
  if (isect == 0) return false;
  // just an optimization; if the intersection contains one of these,
  // we're done because they don't support data.
  if (isect & (BNull | BBool | BArrLikeE | BCStr)) return true;
  // hasData is actually cheaper than mayHaveData, so do those checks first
  if (!hasData() || !o.hasData()) return true;
  // This looks like it could be problematic - eg BCell does not
  // support data, but lots of its subtypes do. It seems like what we
  // need here is !subtypeMayHaveData(isect) (a function we don't
  // actually have). We know however that both inputs have data, so
  // all we rely on here is that if A supports data, and B is a
  // subtype of A that does not (eg TOptArr and TOptArrE), then no
  // subtype of B can support data.
  if (!mayHaveData(isect)) return true;
  return couldBeData(o);
}

bool Type::checkInvariants() const {
  assert(isPredefined(m_bits));
  assert(!hasData() || mayHaveData(m_bits));

#define check(a) \
  if (m_bits & BC##a) assertx(m_bits & BS##a)
  check(Str);
  check(PArrE);
  check(PArrN);
  check(VArrE);
  check(VArrN);
  check(DArrE);
  check(DArrN);
  check(ArrE);
  check(ArrN);
  check(VecE);
  check(VecN);
  check(DictE);
  check(DictN);
  check(KeysetE);
  check(KeysetN);
#undef check

  // NB: Avoid copying non-trivial types in here to avoid recursive calls to
  // checkInvariants() which can cause exponential time blow-ups.

  DEBUG_ONLY auto const& keyType = (m_bits & BSArrLike) == m_bits
    ? TUncArrKey : TArrKey;
  DEBUG_ONLY auto const& valType = (m_bits & BOptArr) == m_bits
    ? TInitGen
    : ((m_bits & BOptKeyset) == m_bits) ? TArrKey : TInitCell;
  DEBUG_ONLY auto const isVArray = (m_bits & BOptVArr) == m_bits;
  DEBUG_ONLY auto const isDArray = (m_bits & BOptDArr) == m_bits;
  DEBUG_ONLY auto const isNotDVArray = (m_bits & BOptPArr) == m_bits;
  DEBUG_ONLY auto const isPHPArray = (m_bits & BOptArr) == m_bits;
  DEBUG_ONLY auto const isVector = (m_bits & BOptVec) == m_bits;
  DEBUG_ONLY auto const isKeyset = (m_bits & BOptKeyset) == m_bits;
  DEBUG_ONLY auto const isDict = (m_bits & BOptDict) == m_bits;
  /*
   * TODO(#3696042): for static arrays, we could enforce that all
   * inner-types are also static (this may would require changes to
   * things like union_of to ensure that we never promote an inner
   * type to a counted type).
   */

  switch (m_dataTag) {
  case DataTag::None:   break;
  case DataTag::Str:    assert(m_data.sval->isStatic()); break;
  case DataTag::Dbl:    break;
  case DataTag::Int:    break;
  case DataTag::RefInner:
    assert(!m_data.inner->couldBe(TRef));
    break;
  case DataTag::Cls:    break;
  case DataTag::Obj:    break;
  case DataTag::ArrLikeVal:
    assert(m_data.aval->isStatic());
    assert(!m_data.aval->empty());
    // If we have a static array, we'd better be sure of the type.
    assert(!isPHPArray || isVArray || isDArray || isNotDVArray);
    assert(!isPHPArray || m_data.aval->isPHPArray());
    assert(!isVArray || m_data.aval->isVArray());
    assert(!isDArray || m_data.aval->isDArray());
    assert(!isNotDVArray || m_data.aval->isNotDVArray());
    assert(!isVector || m_data.aval->isVecArray());
    assert(!isKeyset || m_data.aval->isKeyset());
    assert(!isDict || m_data.aval->isDict());
    break;
  case DataTag::ArrLikePacked: {
    assert(!m_data.packed->elems.empty());
    DEBUG_ONLY auto idx = size_t{0};
    for (DEBUG_ONLY auto const& v : m_data.packed->elems) {
      assert(v.subtypeOf(valType) && v != TBottom);
      assert(!isKeyset || v == ival(idx++));
    }
    break;
  }
  case DataTag::ArrLikeMap: {
    assert(!isVector);
    assert(!isVArray);
    assert(!m_data.map->map.empty());
    DEBUG_ONLY auto idx = size_t{0};
    DEBUG_ONLY auto packed = true;
    for (DEBUG_ONLY auto const& kv : m_data.map->map) {
      assert(cellIsPlausible(kv.first));
      assert(isIntType(kv.first.m_type) ||
             kv.first.m_type == KindOfPersistentString);
      assert(kv.second.subtypeOf(valType) && kv.second != TBottom);
      assert(!isKeyset || from_cell(kv.first) == kv.second);
      if (packed) {
        packed = isIntType(kv.first.m_type) && kv.first.m_data.num == idx;
        ++idx;
      }
    }
    // Map shouldn't have packed-like keys. If it does, it should be Packed
    // instead.
    assert(!packed);
    break;
  }
  case DataTag::ArrLikePackedN:
    assert(m_data.packedn->type.subtypeOf(valType));
    assert(m_data.packedn->type != TBottom);
    assert(!isKeyset || m_data.packedn->type == TInt);
    break;
  case DataTag::ArrLikeMapN:
    assert(!isVector);
    assert(!isVArray);
    assert(m_data.mapn->key.subtypeOf(keyType));
    // MapN shouldn't have a specialized key. If it does, then that implies it
    // only contains arrays of size 1, which means it should be Map instead.
    assert(m_data.mapn->key.m_dataTag == DataTag::None);
    assert(m_data.mapn->val.subtypeOf(valType));
    assert(m_data.mapn->key != TBottom);
    assert(m_data.mapn->val != TBottom);
    assert(!isKeyset || m_data.mapn->key == m_data.mapn->val);
    break;
  }
  return true;
}

//////////////////////////////////////////////////////////////////////

Type wait_handle(const Index& index, Type inner) {
  auto const rwh = index.builtin_class(s_WaitHandle.get());
  auto t = subObj(rwh);
  t.m_data.dobj.whType.emplace(std::move(inner));
  return t;
}

bool is_specialized_wait_handle(const Type& t) {
  return
    t.m_dataTag == DataTag::Obj &&
    t.m_data.dobj.whType;
}

Type wait_handle_inner(const Type& t) {
  assert(is_specialized_wait_handle(t));
  return *t.m_data.dobj.whType;
}

Type sval(SString val) {
  assert(val->isStatic());
  auto r        = Type { BSStr };
  r.m_data.sval = val;
  r.m_dataTag   = DataTag::Str;
  return r;
}

Type ival(int64_t val) {
  auto r        = Type { BInt };
  r.m_data.ival = val;
  r.m_dataTag   = DataTag::Int;
  return r;
}

Type dval(double val) {
  auto r        = Type { BDbl };
  r.m_data.dval = val;
  r.m_dataTag   = DataTag::Dbl;
  return r;
}

Type aval(SArray val) {
  assert(val->isStatic());
  assert(val->isPHPArray());
  if (val->empty()) {
    if (val->isDArray()) return aempty_darray();
    if (val->isVArray()) return aempty_varray();
    return aempty();
  }
  auto r = [&]{
    if (val->isDArray()) return Type { BSDArrN };
    if (val->isVArray()) return Type { BSVArrN };
    return Type { BSPArrN };
  }();
  r.m_data.aval = val;
  r.m_dataTag   = DataTag::ArrLikeVal;
  return r;
}

Type aempty()         { return Type { BSPArrE }; }
Type aempty_varray()  { return Type { BSVArrE }; }
Type aempty_darray()  { return Type { BSDArrE }; }
Type sempty()         { return sval(s_empty.get()); }
Type some_aempty()    { return Type { BPArrE }; }
Type some_aempty_darray() { return Type { BDArrE }; }

Type vec_val(SArray val) {
  assert(val->isStatic());
  assert(val->isVecArray());
  if (val->empty()) return vec_empty();
  auto r        = Type { BSVecN };
  r.m_data.aval = val;
  r.m_dataTag   = DataTag::ArrLikeVal;
  return r;
}

Type vec_empty()         { return Type { BSVecE }; }
Type some_vec_empty()    { return Type { BVecE }; }

Type packedn_impl(trep bits, Type t) {
  auto r = Type { bits };
  construct_inner(r.m_data.packedn, std::move(t));
  r.m_dataTag = DataTag::ArrLikePackedN;
  return r;
}

Type packed_impl(trep bits, std::vector<Type> elems) {
  assert(!elems.empty());
  auto r = Type { bits };
  construct_inner(r.m_data.packed, std::move(elems));
  r.m_dataTag = DataTag::ArrLikePacked;
  return r;
}

Type vec_n(Type ty) {
  return packedn_impl(BVecN, std::move(ty));
}

Type svec_n(Type ty) {
  return packedn_impl(BSVecN, std::move(ty));
}

Type vec(std::vector<Type> elems) {
  return packed_impl(BVecN, std::move(elems));
}

Type svec(std::vector<Type> elems) {
  return packed_impl(BSVecN, std::move(elems));
}

Type dict_val(SArray val) {
  assert(val->isStatic());
  assert(val->isDict());
  if (val->empty()) return dict_empty();
  auto r        = Type { BSDictN };
  r.m_data.aval = val;
  r.m_dataTag   = DataTag::ArrLikeVal;
  return r;
}

Type dict_empty()         { return Type { BSDictE }; }
Type some_dict_empty()    { return Type { BDictE }; }

Type dict_map(MapElems m) {
  return map_impl(BDictN, std::move(m));
}

Type dict_n(Type k, Type v) {
  return mapn_impl(BDictN, std::move(k), std::move(v));
}

Type sdict_n(Type k, Type v) {
  return mapn_impl(BSDictN, std::move(k), std::move(v));
}

Type keyset_val(SArray val) {
  assert(val->isStatic());
  assert(val->isKeyset());
  if (val->empty()) return keyset_empty();
  auto r        = Type { BSKeysetN };
  r.m_data.aval = val;
  r.m_dataTag   = DataTag::ArrLikeVal;
  return r;
}

Type keyset_empty()         { return Type { BSKeysetE }; }
Type some_keyset_empty()    { return Type { BKeysetE }; }

Type keyset_n(Type kv) {
  assert(kv.subtypeOf(TArrKey));
  auto v = kv;
  return mapn_impl(BKeysetN, std::move(kv), std::move(v));
}

Type skeyset_n(Type kv) {
  assert(kv.subtypeOf(TUncArrKey));
  auto v = kv;
  return mapn_impl(BSKeysetN, std::move(kv), std::move(v));
}

Type subObj(res::Class val) {
  auto r = Type { BObj };
  construct(r.m_data.dobj,
            val.couldBeOverriden() ? DObj::Sub : DObj::Exact,
            val);
  r.m_dataTag = DataTag::Obj;
  return r;
}

Type objExact(res::Class val) {
  auto r = Type { BObj };
  construct(r.m_data.dobj, DObj::Exact, val);
  r.m_dataTag = DataTag::Obj;
  return r;
}

Type subCls(res::Class val) {
  auto r        = Type { BCls };
  construct(r.m_data.dcls,
            val.couldBeOverriden() ? DCls::Sub : DCls::Exact,
            val);
  r.m_dataTag = DataTag::Cls;
  return r;
}

Type clsExact(res::Class val) {
  auto r        = Type { BCls };
  construct(r.m_data.dcls, DCls::Exact, val);
  r.m_dataTag   = DataTag::Cls;
  return r;
}


Type ref_to(Type t) {
  assert(t.subtypeOf(TInitCell));
  auto r = Type{BRef};
  construct_inner(r.m_data.inner, std::move(t));
  r.m_dataTag = DataTag::RefInner;
  return r;
}

bool is_ref_with_inner(const Type& t) {
  return t.m_dataTag == DataTag::RefInner;
}

bool is_specialized_array_like(const Type& t) {
  switch (t.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::Obj:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
  case DataTag::RefInner:
    return false;
  case DataTag::ArrLikeVal:
  case DataTag::ArrLikePacked:
  case DataTag::ArrLikePackedN:
  case DataTag::ArrLikeMap:
  case DataTag::ArrLikeMapN:
    return true;
  }
  not_reached();
}

bool is_specialized_array(const Type& t) {
  return t.subtypeOf(TOptArr) && is_specialized_array_like(t);
}

bool is_specialized_vec(const Type& t) {
  return t.subtypeOf(TOptVec) && is_specialized_array_like(t);
}

bool is_specialized_dict(const Type& t) {
  return t.subtypeOf(TOptDict) && is_specialized_array_like(t);
}

bool is_specialized_keyset(const Type& t) {
  return t.subtypeOf(TOptKeyset) && is_specialized_array_like(t);
}

Type set_trep(Type& a, trep bits) {
  // If the type and its new bits don't agree on d/varray-ness and the type has
  // a static array, we need to convert the static array into its equivalent
  // Packed or Map type. We cannot have a ArrLikeVal if the type isn't
  // specifically a subtype of TOptParr, TOptVArr, or TOptDArr.
  if (a.m_dataTag == DataTag::ArrLikeVal &&
      ((a.subtypeOf(TOptPArr) && ((bits & BOptPArr) != bits)) ||
       (a.subtypeOf(TOptVArr) && ((bits & BOptVArr) != bits)) ||
       (a.subtypeOf(TOptDArr) && ((bits & BOptDArr) != bits)))) {
    if (auto p = toDArrLikePacked(a.m_data.aval)) {
      return packed_impl(bits, std::move(p->elems));
    }
    auto d = toDArrLikeMap(a.m_data.aval);
    return map_impl(bits, std::move(d->map));
  }
  a.m_bits = bits;
  return std::move(a);
}

/*
 * Helper function for unioning a specialized array-like type, spec_a, with a
 * same-array-kind type b.
 *
 * opt_e and opt are the corresponding non-specialized nullable-empty,
 * and nullable array-like types (ie OptArrE and OptArr, or OptVecE
 * and OptVec).
 */
Type spec_array_like_union(Type& spec_a,
                           Type& b,
                           const Type& opt_e,
                           const Type& opt) {
  // If b isn't the same kind of array-like, we'll have to treat it as
  // a union of two separate types
  if (!b.subtypeOf(opt)) return TBottom;
  auto const bits = combine_arr_like_bits(spec_a.m_bits, b.m_bits);
  if (!is_specialized_array_like(b)) {
    // We can keep a's specialization if b is an empty array-like
    // or a nullable empty array-like.
    if (b.subtypeOf(opt_e)) return set_trep(spec_a, bits);
    // otherwise drop the specialized bits
    return Type { bits };
  }

  DEBUG_ONLY auto const shouldBeOpt = is_opt(spec_a) || is_opt(b);
  auto const t = Type::unionArrLike(std::move(spec_a), std::move(b));
  assert(!shouldBeOpt || is_opt(t));
  return t;
}

Type arr_packed(std::vector<Type> elems) {
  return packed_impl(BPArrN, std::move(elems));
}

Type arr_packed_varray(std::vector<Type> elems) {
  return packed_impl(BVArrN, std::move(elems));
}

Type sarr_packed(std::vector<Type> elems) {
  return packed_impl(BSPArrN, std::move(elems));
}

Type arr_packedn(Type t) {
  return packedn_impl(BPArrN, std::move(t));
}

Type sarr_packedn(Type t) {
  return packedn_impl(BSPArrN, std::move(t));
}

Type map_impl(trep bits, MapElems m) {
  assert(!m.empty());

  // A Map cannot be packed, so if it is, return a Packed instead.
  auto idx = int64_t{0};
  auto packed = true;
  for (auto const& p : m) {
    if (!isIntType(p.first.m_type) || p.first.m_data.num != idx) {
      packed = false;
      break;
    }
    ++idx;
  }
  if (packed) {
    std::vector<Type> elems;
    for (auto& p : m) elems.emplace_back(std::move(p.second));
    return packed_impl(bits, std::move(elems));
  }

  auto r = Type { bits };
  construct_inner(r.m_data.map, std::move(m));
  r.m_dataTag = DataTag::ArrLikeMap;
  return r;
}

Type arr_map(MapElems m) {
  return map_impl(BPArrN, std::move(m));
}

Type arr_map_darray(MapElems m) {
  return map_impl(BDArrN, std::move(m));
}

Type sarr_map(MapElems m) {
  return map_impl(BSPArrN, std::move(m));
}

Type mapn_impl(trep bits, Type k, Type v) {
  assert(k.subtypeOf(TArrKey));

  // A MapN cannot have a constant key (because that can actually make it be a
  // subtype of Map sometimes), so if it does, make it a Map instead.
  if (auto val = tv(k)) {
    MapElems m;
    m.emplace_back(*val, std::move(v));
    return map_impl(bits, std::move(m));
  }

  auto r = Type { bits };
  construct_inner(
    r.m_data.mapn,
    std::move(k),
    std::move(v)
  );
  r.m_dataTag = DataTag::ArrLikeMapN;
  return r;
}

Type arr_mapn(Type k, Type v) {
  return mapn_impl(BPArrN, std::move(k), std::move(v));
}

Type sarr_mapn(Type k, Type v) {
  return mapn_impl(BSPArrN, std::move(k), std::move(v));
}

Type opt(Type t) {
  assert(canBeOptional(t.m_bits));
  auto ret = t;
  ret.m_bits = static_cast<trep>(ret.m_bits | BInitNull);
  return ret;
}

Type unopt(Type t) {
  assert(is_opt(t));
  t.m_bits = static_cast<trep>(t.m_bits & ~BInitNull);
  assert(!is_opt(t));
  return t;
}

bool is_opt(const Type& t) {
  if (t.m_bits == BInitNull) return false;
  if (!t.couldBe(TInitNull)) return false;
  auto const nonNullBits = static_cast<trep>(t.m_bits & ~BInitNull);
  return isPredefined(nonNullBits) && canBeOptional(nonNullBits);
}

bool is_specialized_obj(const Type& t) {
  return t.m_dataTag == DataTag::Obj;
}

bool is_specialized_cls(const Type& t) {
  return t.m_dataTag == DataTag::Cls;
}

Type objcls(const Type& t) {
  if (t.subtypeOf(TObj) && is_specialized_obj(t)) {
    auto const d = dobj_of(t);
    return d.type == DObj::Exact ? clsExact(d.cls) : subCls(d.cls);
  }
  return TCls;
}

//////////////////////////////////////////////////////////////////////

folly::Optional<int64_t> arr_size(const Type& t) {
  switch (t.m_dataTag) {
    case DataTag::ArrLikeVal:
      return t.m_data.aval->size();

    case DataTag::ArrLikeMap:
      return t.m_data.map->map.size();

    case DataTag::ArrLikePacked:
      return t.m_data.packed->elems.size();

    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::RefInner:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::Cls:
      return folly::none;
  }
  not_reached();
}

Type::ArrayCat categorize_array(const Type& t) {
  auto hasInts = false;
  auto hasStrs = false;
  auto isPacked = true;
  // Even if all the values are constants, we can't produce a constant array
  // unless the d/varray-ness is definitely known.
  auto val = t.subtypeOfAny(TPArr, TVArr, TDArr);
  size_t idx = 0;
  auto checkKey = [&] (const Cell& key) {
    if (isStringType(key.m_type)) {
      hasStrs = true;
      isPacked = false;
      return hasInts;
    } else {
      hasInts = true;
      if (key.m_data.num != idx++) isPacked = false;
      return hasStrs && !isPacked;
    }
  };

  switch (t.m_dataTag) {
    case DataTag::ArrLikeVal:
      IterateKV(t.m_data.aval,
                [&] (Cell k, TypedValue) {
                  return checkKey(k);
                });
      break;

    case DataTag::ArrLikeMap:
      for (auto const& elem : t.m_data.map->map) {
        if (checkKey(elem.first) && !val) break;
        val = val && tv(elem.second);
      }
      break;

    case DataTag::ArrLikePacked:
      for (auto const& elem : t.m_data.packed->elems) {
        hasInts = true;
        val = val && tv(elem);
        if (!val) break;
      }
      break;

    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::RefInner:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::Cls:
      return {};
  }

  auto cat =
    hasInts ? (isPacked ? Type::ArrayCat::Packed : Type::ArrayCat::Mixed) :
    hasStrs ? Type::ArrayCat::Struct : Type::ArrayCat::Empty;

  return { cat , val };
}

CompactVector<LSString> get_string_keys(const Type& t) {
  CompactVector<LSString> strs;

  switch (t.m_dataTag) {
    case DataTag::ArrLikeVal:
      IterateKV(t.m_data.aval,
                [&] (Cell k, TypedValue) {
                  assert(isStringType(k.m_type));
                  strs.push_back(k.m_data.pstr);
                });
      break;

    case DataTag::ArrLikeMap:
      for (auto const& elem : t.m_data.map->map) {
        assert(isStringType(elem.first.m_type));
        strs.push_back(elem.first.m_data.pstr);
      }
      break;

    case DataTag::ArrLikePacked:
    case DataTag::None:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
    case DataTag::RefInner:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::Cls:
      always_assert(false);
  }

  return strs;
}

template<typename R>
struct tvHelper {
  template<DataType dt,typename... Args>
  static R make(Args&&... args) {
    return make_tv<dt>(std::forward<Args>(args)...);
  }
  template<typename Init, typename... Args>
  static R fromMap(Args&&... args) {
    return fromTypeMap<Init>(std::forward<Args>(args)...);
  }
  template<typename Init, typename... Args>
  static R fromVec(Args&&... args) {
    return fromTypeVec<Init>(std::forward<Args>(args)...);
  }
};

template<>
struct tvHelper<bool> {
  template<DataType dt,typename... Args>
  static bool make(Args&&... args) {
    return true;
  }
  template<typename Init, typename... Args>
  static bool fromMap(Args&&... args) {
    return checkTypeMap(std::forward<Args>(args)...);
  }
  template<typename Init, typename... Args>
  static bool fromVec(Args&&... args) {
    return checkTypeVec(std::forward<Args>(args)...);
  }
};

template<typename R>
R tvImpl(const Type& t) {
  assert(t.checkInvariants());
  using H = tvHelper<R>;

  switch (t.m_bits) {
  case BUninit:      return H::template make<KindOfUninit>();
  case BInitNull:    return H::template make<KindOfNull>();
  case BTrue:        return H::template make<KindOfBoolean>(true);
  case BFalse:       return H::template make<KindOfBoolean>(false);
  case BPArrE:
  case BSPArrE:
    return H::template make<KindOfPersistentArray>(staticEmptyArray());
  case BVArrE:
  case BSVArrE:
    return H::template make<KindOfPersistentArray>(staticEmptyVArray());
  case BDArrE:
  case BSDArrE:
    return H::template make<KindOfPersistentArray>(staticEmptyDArray());
  case BVecE:
  case BSVecE:
    return H::template make<KindOfPersistentVec>(staticEmptyVecArray());
  case BDictE:
  case BSDictE:
    return H::template make<KindOfPersistentDict>(staticEmptyDictArray());
  case BKeysetE:
  case BSKeysetE:
    return H::template make<KindOfPersistentKeyset>(staticEmptyKeysetArray());

  case BCStr:
  case BCArrE:
  case BCArrN:
  case BCArr:
  case BCPArrE:
  case BCPArrN:
  case BCPArr:
  case BCVArrE:
  case BCVArrN:
  case BCVArr:
  case BCDArrE:
  case BCDArrN:
  case BCDArr:
  case BCVecE:
  case BCVecN:
  case BCVec:
  case BCDictE:
  case BCDictN:
  case BCDict:
  case BCKeysetE:
  case BCKeysetN:
  case BCKeyset:
    // We don't produce these types.
    always_assert(false);

  default:
    if (is_opt(t)) {
      break;
    }
    switch (t.m_dataTag) {
    case DataTag::Int:
      return H::template make<KindOfInt64>(t.m_data.ival);
    case DataTag::Dbl:
      return H::template make<KindOfDouble>(t.m_data.dval);
    case DataTag::Str:
      return H::template make<KindOfPersistentString>(t.m_data.sval);
    case DataTag::ArrLikeVal:
      if ((t.m_bits & BArrN) == t.m_bits) {
        return H::template make<KindOfPersistentArray>(
          const_cast<ArrayData*>(t.m_data.aval)
        );
      }
      if ((t.m_bits & BVecN) == t.m_bits) {
        return H::template make<KindOfPersistentVec>(
          const_cast<ArrayData*>(t.m_data.aval)
        );
      }
      if ((t.m_bits & BDictN) == t.m_bits) {
        return H::template make<KindOfPersistentDict>(
          const_cast<ArrayData*>(t.m_data.aval)
        );
      }
      if ((t.m_bits & BKeysetN) == t.m_bits) {
        return H::template make<KindOfPersistentKeyset>(
          const_cast<ArrayData*>(t.m_data.aval)
        );
      }
      break;
    case DataTag::ArrLikeMap:
      if ((t.m_bits & BDictN) == t.m_bits) {
        return H::template fromMap<DictInit>(t.m_data.map->map);
      } else if ((t.m_bits & BKeysetN) == t.m_bits) {
        return H::template fromMap<KeysetInit>(t.m_data.map->map);
      } else if ((t.m_bits & BPArrN) == t.m_bits) {
        return H::template fromMap<MixedArrayInit>(t.m_data.map->map);
      } else if ((t.m_bits & BDArrN) == t.m_bits) {
        return H::template fromMap<DArrayInit>(t.m_data.map->map);
      }
      break;
    case DataTag::ArrLikePacked:
      if ((t.m_bits & BVecN) == t.m_bits) {
        return H::template fromVec<VecArrayInit>(t.m_data.packed->elems);
      } else if ((t.m_bits & BDictN) == t.m_bits) {
        return H::template fromVec<DictInit>(t.m_data.packed->elems);
      } else if ((t.m_bits & BKeysetN) == t.m_bits) {
        return H::template fromVec<KeysetAppendInit>(t.m_data.packed->elems);
      } else if ((t.m_bits & BPArrN) == t.m_bits) {
        return H::template fromVec<PackedArrayInit>(t.m_data.packed->elems);
      } else if ((t.m_bits & BVArrN) == t.m_bits) {
        return H::template fromVec<VArrayInit>(t.m_data.packed->elems);
      } else if ((t.m_bits & BDArrN) == t.m_bits) {
        return H::template fromVec<DArrayInit>(t.m_data.packed->elems);
      }
      break;
    case DataTag::RefInner:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::Cls:
    case DataTag::None:
      break;
    }
  }

  return R{};
}

folly::Optional<Cell> tv(const Type& t) {
  return tvImpl<folly::Optional<Cell>>(t);
}

bool is_scalar(const Type& t) {
  return tvImpl<bool>(t);
}

Type scalarize(Type t) {
  assertx(is_scalar(t));

  switch (t.m_dataTag) {
    case DataTag::None:
      assertx(t.subtypeOfAny(TNull, TTrue, TFalse,
                             TArrE, TVecE, TDictE, TKeysetE));
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Str:
      return t;
    case DataTag::ArrLikeVal:
      t.m_bits = static_cast<trep>(t.m_bits &
                                   (BSArrN | BSVecN | BSDictN | BSKeysetN));
      return t;
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikePacked:
      return from_cell(*tv(t));
    case DataTag::RefInner:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMapN:
    case DataTag::Obj:
    case DataTag::Cls:
      break;
  }
  not_reached();
}

Type type_of_istype(IsTypeOp op) {
  switch (op) {
  case IsTypeOp::Uninit: return TUninit;
  case IsTypeOp::Null:   return TNull;
  case IsTypeOp::Bool:   return TBool;
  case IsTypeOp::Int:    return TInt;
  case IsTypeOp::Dbl:    return TDbl;
  case IsTypeOp::Str:    return TStr;
  case IsTypeOp::Arr:    return TArr;
  case IsTypeOp::Vec:    return TVec;
  case IsTypeOp::Dict:   return TDict;
  case IsTypeOp::Keyset: return TKeyset;
  case IsTypeOp::Obj:    return TObj;
  case IsTypeOp::VArray: return TVArr;
  case IsTypeOp::DArray: return TDArr;
  case IsTypeOp::Scalar: always_assert(0);
  }
  not_reached();
}

DObj dobj_of(const Type& t) {
  assert(t.checkInvariants());
  assert(is_specialized_obj(t));
  return t.m_data.dobj;
}

DCls dcls_of(Type t) {
  assert(t.checkInvariants());
  assert(is_specialized_cls(t));
  return t.m_data.dcls;
}

Type from_cell(Cell cell) {
  assert(cellIsPlausible(cell));

  switch (cell.m_type) {
  case KindOfUninit:   return TUninit;
  case KindOfNull:     return TInitNull;
  case KindOfBoolean:  return cell.m_data.num ? TTrue : TFalse;
  case KindOfInt64:    return ival(cell.m_data.num);
  case KindOfDouble:   return dval(cell.m_data.dbl);

  case KindOfPersistentString:
  case KindOfString:
    always_assert(cell.m_data.pstr->isStatic());
    return sval(cell.m_data.pstr);

  case KindOfPersistentVec:
  case KindOfVec:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isVecArray());
    return vec_val(cell.m_data.parr);

  case KindOfPersistentDict:
  case KindOfDict:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isDict());
    return dict_val(cell.m_data.parr);

  case KindOfPersistentKeyset:
  case KindOfKeyset:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isKeyset());
    return keyset_val(cell.m_data.parr);

  case KindOfPersistentArray:
  case KindOfArray:
    always_assert(cell.m_data.parr->isStatic());
    always_assert(cell.m_data.parr->isPHPArray());
    return aval(cell.m_data.parr);

  case KindOfRef:
  case KindOfObject:
  case KindOfResource:
    break;
  }
  always_assert(0 && "reference counted/class type in from_cell");
}

Type from_DataType(DataType dt) {
  switch (dt) {
  case KindOfUninit:   return TUninit;
  case KindOfNull:     return TInitNull;
  case KindOfBoolean:  return TBool;
  case KindOfInt64:    return TInt;
  case KindOfDouble:   return TDbl;
  case KindOfPersistentString:
  case KindOfString:   return TStr;
  case KindOfPersistentVec:
  case KindOfVec:      return TVec;
  case KindOfPersistentDict:
  case KindOfDict:     return TDict;
  case KindOfPersistentKeyset:
  case KindOfKeyset:   return TKeyset;
  case KindOfPersistentArray:
  case KindOfArray:    return TArr;
  case KindOfRef:      return TRef;
  case KindOfObject:   return TObj;
  case KindOfResource: return TRes;
    break;
  }
  always_assert(0 && "dt in from_DataType didn't satisfy preconditions");
}

Type from_hni_constraint(SString s) {
  if (!s) return TGen;

  auto p   = s->data();
  auto ret = TBottom;

  if (*p == '?') {
    ret |= TInitNull;
    ++p;
  }

  if (!strcasecmp(p, "HH\\resource")) return union_of(ret, TRes);
  if (!strcasecmp(p, "HH\\bool"))     return union_of(ret, TBool);
  if (!strcasecmp(p, "HH\\int"))      return union_of(ret, TInt);
  if (!strcasecmp(p, "HH\\float"))    return union_of(ret, TDbl);
  if (!strcasecmp(p, "HH\\num"))      return union_of(ret, TNum);
  if (!strcasecmp(p, "HH\\string"))   return union_of(ret, TStr);
  if (!strcasecmp(p, "HH\\dict"))     return union_of(ret, TDict);
  if (!strcasecmp(p, "HH\\vec"))      return union_of(ret, TVec);
  if (!strcasecmp(p, "HH\\keyset"))   return union_of(ret, TKeyset);
  if (!strcasecmp(p, "HH\\varray"))   return union_of(ret, TArr);
  if (!strcasecmp(p, "HH\\darray"))   return union_of(ret, TArr);
  if (!strcasecmp(p, "HH\\varray_or_darray"))   return union_of(ret, TArr);
  if (!strcasecmp(p, "array"))        return union_of(ret, TArr);
  if (!strcasecmp(p, "HH\\arraykey")) return union_of(ret, TArrKey);
  if (!strcasecmp(p, "HH\\mixed"))    return TInitGen;

  // It might be an object, or we might want to support type aliases in HNI at
  // some point.  For now just be conservative.
  return TGen;
}

Type intersection_of(Type a, Type b) {
  auto const isect = static_cast<trep>(a.m_bits & b.m_bits);
  if (!mayHaveData(isect)) return Type { isect };

  auto fix = [&] (Type& t) {
    t.m_bits = isect;
    return std::move(t);
  };

  if (!b.hasData())     return fix(a);
  if (!a.hasData())     return fix(b);
  if (a.subtypeData(b)) return fix(a);
  if (b.subtypeData(a)) return fix(b);

  auto t = [&] {
    if (a.m_dataTag == b.m_dataTag) {
      switch (a.m_dataTag) {
        case DataTag::None:
          not_reached();
        case DataTag::Obj:
        {
          auto fixWh = [&] (Type& t) {
            if (!a.m_data.dobj.whType) {
              t.m_data.dobj.whType = b.m_data.dobj.whType;
            } else if (!b.m_data.dobj.whType) {
              t.m_data.dobj.whType = a.m_data.dobj.whType;
            } else {
              auto whType = intersection_of(*a.m_data.dobj.whType,
                                            *b.m_data.dobj.whType);
              if (whType == TBottom) return TBottom;
              *t.m_data.dobj.whType.mutate() = whType;
            }
            return fix(t);
          };
          if (a.m_data.dobj.type == b.m_data.dobj.type &&
              a.m_data.dobj.cls.same(b.m_data.dobj.cls)) {
            return fixWh(a);
          }
          if (b.m_data.dobj.type == DObj::Sub &&
              a.m_data.dobj.cls.subtypeOf(b.m_data.dobj.cls)) {
            return fixWh(a);
          }
          if (a.m_data.dobj.type == DObj::Sub &&
              b.m_data.dobj.cls.subtypeOf(a.m_data.dobj.cls)) {
            return fixWh(b);
          }
          if (a.m_data.dobj.type == DObj::Sub &&
              b.m_data.dobj.type == DObj::Sub) {
            if (a.m_data.dobj.cls.couldBeInterface()) {
              if (!b.m_data.dobj.cls.couldBeInterface()) {
                return fixWh(b);
              } else {
                return Type { isect };
              }
            } else if (b.m_data.dobj.cls.couldBeInterface()) {
              return fixWh(a);
            }
          }
          return TBottom;
        }
        case DataTag::Cls:
        case DataTag::Str:
        case DataTag::ArrLikeVal:
        case DataTag::Int:
        case DataTag::Dbl:
          // Neither is a subtype of the other, so the intersection is empty
          return TBottom;
        case DataTag::RefInner:
        {
          auto inner = intersection_of(*a.m_data.inner, *b.m_data.inner);
          if (inner == TBottom) return TBottom;
          *a.m_data.inner.mutate() = inner;
          return fix(a);
        }
        case DataTag::ArrLikePacked:
        case DataTag::ArrLikePackedN:
        case DataTag::ArrLikeMap:
        case DataTag::ArrLikeMapN:
          // will be handled by dual dispatch.
          break;
      }
    }
    return a.dualDispatchDataFn(b, DualDispatchIntersection{ isect });
  }();

  if (t != TBottom) return t;
  auto const bits =
    isect & ~(BInt|BDbl|BSStr|BArrN|BVecN|BDictN|BKeysetN|BObj|BRef);
  return Type { static_cast<trep>(bits) };
}

Type Type::unionArrLike(Type a, Type b) {
  auto const newBits = combine_arr_like_bits(a.m_bits, b.m_bits);
  if (a.subtypeData(b)) return set_trep(b, newBits);
  if (b.subtypeData(a)) return set_trep(a, newBits);
  return a.dualDispatchDataFn(b, DualDispatchUnion{ newBits });
}

Type union_of(Type a, Type b) {
  if (a.subtypeOf(b)) return b;
  if (b.subtypeOf(a)) return a;

  /*
   * We need to check this before specialized objects, including the case where
   * one of them was TInitNull, because otherwise we'll go down the
   * is_specialized_obj paths and lose the wait handle information.
   */
  if (is_specialized_wait_handle(a)) {
    if (is_specialized_wait_handle(b)) {
      *a.m_data.dobj.whType.mutate() |= *b.m_data.dobj.whType;
      return a;
    }
    if (b == TInitNull) return opt(a);
  }
  if (is_specialized_wait_handle(b)) {
    if (a == TInitNull) return opt(b);
  }

  // When both types are strict subtypes of TObj or TOptObj or both
  // are strict subtypes of TCls we look for a common ancestor if one
  // exists.
  if (is_specialized_obj(a) && is_specialized_obj(b)) {
    auto keepOpt = is_opt(a) || is_opt(b);
    auto t = a.m_data.dobj.cls.commonAncestor(dobj_of(b).cls);
    // We need not to distinguish between Obj<=T and Obj=T, and always
    // return an Obj<=Ancestor, because that is the single type that
    // includes both children.
    if (t) return keepOpt ? opt(subObj(*t)) : subObj(*t);
    return keepOpt ? TOptObj : TObj;
  }
  if (a.strictSubtypeOf(TCls) && b.strictSubtypeOf(TCls)) {
    auto t = a.m_data.dcls.cls.commonAncestor(dcls_of(b).cls);
    // Similar to above, this must always return an Obj<=Ancestor.
    return t ? subCls(*t) : TCls;
  }

  if (is_specialized_array(a)) {
    auto t = spec_array_like_union(a, b, TOptArrE, TOptArr);
    if (t != TBottom) return t;
  } else if (is_specialized_array(b)) {
    auto t = spec_array_like_union(b, a, TOptArrE, TOptArr);
    if (t != TBottom) return t;
  }

  if (is_specialized_vec(a)) {
    auto t = spec_array_like_union(a, b, TOptVecE, TOptVec);
    if (t != TBottom) return t;
  } else if (is_specialized_vec(b)) {
    auto t = spec_array_like_union(b, a, TOptVecE, TOptVec);
    if (t != TBottom) return t;
  }

  if (is_specialized_dict(a)) {
    auto t = spec_array_like_union(a, b, TOptDictE, TOptDict);
    if (t != TBottom) return t;
  } else if (is_specialized_dict(b)) {
    auto t = spec_array_like_union(b, a, TOptDictE, TOptDict);
    if (t != TBottom) return t;
  }

  if (is_specialized_keyset(a)) {
    auto t = spec_array_like_union(a, b, TOptKeysetE, TOptKeyset);
    if (t != TBottom) return t;
  } else if (is_specialized_keyset(b)) {
    auto t = spec_array_like_union(b, a, TOptKeysetE, TOptKeyset);
    if (t != TBottom) return t;
  }

  if (is_ref_with_inner(a) && is_ref_with_inner(b)) {
    return ref_to(union_of(*a.m_data.inner, *b.m_data.inner));
  }

#define X(y) if (a.subtypeOf(y) && b.subtypeOf(y)) return y;
  X(TInt)
  X(TDbl)
  X(TSStr)

  X(TSPArr)
  X(TPArrE)
  X(TPArrN)
  X(TPArr)

  X(TSVArr)
  X(TVArrE)
  X(TVArrN)
  X(TVArr)

  X(TSDArr)
  X(TDArrE)
  X(TDArrN)
  X(TDArr)

  X(TSArrE)
  X(TSArrN)
  X(TSArr)
  X(TArrE)
  X(TArrN)
  X(TArr)

  X(TObj)
  X(TCls)
  X(TNull)
  X(TBool)
  X(TNum)
  X(TStr)

  X(TSVec)
  X(TVecE)
  X(TVecN)
  X(TVec)
  X(TSDict)
  X(TDictE)
  X(TDictN)
  X(TDict)
  X(TSKeyset)
  X(TKeysetE)
  X(TKeysetN)
  X(TKeyset)

  X(TUncArrKey)
  X(TArrKey)

  /*
   * Merging option types tries to preserve subtype information where it's
   * possible.  E.g. if you union InitNull and Obj<=Foo, we want OptObj<=Foo to
   * be the result.
   */
  if (a == TInitNull && canBeOptional(b.m_bits)) return opt(b);
  if (b == TInitNull && canBeOptional(a.m_bits)) return opt(a);

  // Optional types where the non-Null part is already a union or can
  // have a value need to be manually tried (e.g. if we are merging
  // TOptTrue and TOptFalse, we want TOptBool, or merging TOptInt=1
  // and TOptInt=2 should give us TOptInt).
  X(TOptBool)
  X(TOptInt)
  X(TOptDbl)
  X(TOptNum)
  X(TOptSStr)
  X(TOptStr)
  X(TOptObj)

  X(TOptSPArr)
  X(TOptPArrE)
  X(TOptPArrN)
  X(TOptPArr)

  X(TOptSVArr)
  X(TOptVArrE)
  X(TOptVArrN)
  X(TOptVArr)

  X(TOptSDArr)
  X(TOptDArrE)
  X(TOptDArrN)
  X(TOptDArr)

  X(TOptSArrE)
  X(TOptSArrN)
  X(TOptSArr)
  X(TOptArrE)
  X(TOptArrN)
  X(TOptArr)

  X(TOptSVec)
  X(TOptVecE)
  X(TOptVecN)
  X(TOptVec)
  X(TOptSDict)
  X(TOptDictE)
  X(TOptDictN)
  X(TOptDict)
  X(TOptSKeyset)
  X(TOptKeysetE)
  X(TOptKeysetN)
  X(TOptKeyset)

  X(TOptUncArrKey)
  X(TOptArrKey)

  X(TInitPrim)
  X(TPrim)
  X(TInitUnc)
  X(TUnc)
  X(TInitCell)
  X(TCell)
  X(TInitGen)
  X(TGen)
#undef X

  return TTop;
}

Type promote_emptyish(Type a, Type b) {
  if (is_opt(a)) a = unopt(a);
  if (a.subtypeOf(sempty())) {
    return b;
  }
  auto t = trep(a.m_bits & ~(BNull | BFalse));
  if (!isPredefined(t)) {
    if (trep(t & BInitPrim) == t) {
      t = BInitPrim;
    } else if (trep(t & BInitUnc) == t) {
      t = BInitUnc;
    } else if (trep(t & BInitCell) == t) {
      t = BInitCell;
    } else {
      t = BInitGen;
    }
    return union_of(Type { t }, b);
  }
  a.m_bits = t;
  return union_of(a, b);
}

bool could_have_magic_bool_conversion(const Type& t) {
  if (!t.couldBe(TObj)) return false;

  if (t.strictSubtypeOf(TObj) ||
      (is_opt(t) && unopt(t).strictSubtypeOf(TObj))) {
    return dobj_of(t).cls.couldHaveMagicBool();
  }
  return true;
}

Emptiness emptiness(const Type& t) {
  auto const empty_mask = BNull | BFalse | BArrE | BVecE | BDictE | BKeysetE;
  if ((t.m_bits & empty_mask) == t.m_bits) return Emptiness::Empty;
  auto const non_empty_mask = BTrue | BArrN | BVecN | BDictN | BKeysetN;
  if ((t.m_bits & non_empty_mask) == t.m_bits) return Emptiness::NonEmpty;
  if (t.strictSubtypeOf(TObj)) {
    if (!could_have_magic_bool_conversion(t)) {
      return Emptiness::NonEmpty;
    }
  } else if (auto v = tv(t)) {
    return cellToBool(*v) ? Emptiness::NonEmpty : Emptiness::Empty;
  }

  return Emptiness::Maybe;
}

void widen_type_impl(Type& t, uint32_t depth) {
  // Right now to guarantee termination we need to just limit the nesting depth
  // of the type to a fixed degree.
  auto const checkDepth = [&]{
    if (depth >= kTypeWidenMaxDepth) {
      t = Type { t.m_bits };
      return true;
    }
    return false;
  };

  switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Str:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Cls:
    case DataTag::ArrLikeVal:
      return;

    case DataTag::Obj:
      if (t.m_data.dobj.whType) {
        widen_type_impl(*t.m_data.dobj.whType.mutate(), depth + 1);
      }
      return;

    case DataTag::RefInner:
      return widen_type_impl(*t.m_data.inner.mutate(), depth + 1);

    case DataTag::ArrLikePacked: {
      if (checkDepth()) return;
      auto& packed = *t.m_data.packed.mutate();
      for (auto& e : packed.elems) {
        widen_type_impl(e, depth + 1);
      }
      return;
    }

    case DataTag::ArrLikePackedN: {
      if (checkDepth()) return;
      auto& packed = *t.m_data.packedn.mutate();
      widen_type_impl(packed.type, depth + 1);
      return;
    }

    case DataTag::ArrLikeMap: {
      if (checkDepth()) return;
      auto& map = *t.m_data.map.mutate();
      for (auto it = map.map.begin(); it != map.map.end(); it++) {
        auto temp = it->second;
        widen_type_impl(temp, depth + 1);
        map.map.update(it, std::move(temp));
      }
      return;
    }

    case DataTag::ArrLikeMapN: {
      if (checkDepth()) return;
      auto& map = *t.m_data.mapn.mutate();
      // Key must be at least ArrKey, which doesn't need widening.
      widen_type_impl(map.val, depth + 1);
      return;
    }
  }

  not_reached();
}

Type widen_type(Type t) {
  widen_type_impl(t, 0);
  return t;
}

Type widening_union(const Type& a, const Type& b) {
  if (a.subtypeOf(b)) return b;
  if (b.subtypeOf(a)) return a;
  return widen_type(union_of(a, b));
}

Type stack_flav(Type a) {
  if (a.subtypeOf(TUninit))   return TUninit;
  if (a.subtypeOf(TInitCell)) return TInitCell;
  if (a.subtypeOf(TRef))      return TRef;
  if (a.subtypeOf(TGen))      return TGen;
  always_assert(0 && "stack_flav passed invalid type");
}

Type loosen_staticness(Type t) {
  auto const check = [&](trep a){
    if (t.m_bits & a) t.m_bits = static_cast<trep>(t.m_bits | a);
  };
  // Need to remove any constant value from a string because a TStr cannot have
  // one.
  if (t.couldBe(TStr)) t |= TStr;
  check(BPArrE);
  check(BPArrN);
  check(BVArrE);
  check(BVArrN);
  check(BDArrE);
  check(BDArrN);
  check(BVecE);
  check(BVecN);
  check(BDictE);
  check(BDictN);
  check(BKeysetE);
  check(BKeysetN);
  return t;
}

Type loosen_dvarrayness(Type t) {
  auto const check = [&](trep a) {
    if (t.m_bits & a) t.m_bits = static_cast<trep>(t.m_bits | a);
  };
  if (t.couldBe(TArr) && t.m_dataTag == DataTag::ArrLikeVal) {
    // We need to drop any static array from the type because TArr unions cannot
    // have one. Turn it into the equivalent Packed or Map data.
    if (auto p = toDArrLikePacked(t.m_data.aval)) {
      t = packed_impl(t.m_bits, std::move(p->elems));
    } else {
      auto d = toDArrLikeMap(t.m_data.aval);
      t = map_impl(t.m_bits, std::move(d->map));
    }
  }
  check(BSArrE);
  check(BCArrE);
  check(BSArrN);
  check(BCArrN);
  return t;
}

Type loosen_arrays(Type a) {
  if (a.couldBe(TArr))    a |= TArr;
  if (a.couldBe(TVec))    a |= TVec;
  if (a.couldBe(TDict))   a |= TDict;
  if (a.couldBe(TKeyset)) a |= TKeyset;
  return a;
}

Type loosen_values(Type a) {
  auto t = [&]{
    switch (a.m_dataTag) {
    case DataTag::Str:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::RefInner:
    case DataTag::ArrLikeVal:
    case DataTag::ArrLikePacked:
    case DataTag::ArrLikePackedN:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
      return Type { a.m_bits };
    case DataTag::None:
    case DataTag::Obj:
    case DataTag::Cls:
      return a;
    }
    not_reached();
  }();
  if (t.couldBe(TFalse) || t.couldBe(TTrue)) t |= TBool;
  return t;
}

Type loosen_emptiness(Type t) {
  auto const check = [&](trep a, trep b){
    if (t.m_bits & a) t.m_bits = static_cast<trep>(t.m_bits | b);
  };
  check(BSPArr,   BSPArr);
  check(BCPArr,   BPArr);
  check(BSVArr,   BSVArr);
  check(BCVArr,   BVArr);
  check(BSDArr,   BSDArr);
  check(BCDArr,   BDArr);
  check(BSVec,    BSVec);
  check(BCVec,    BVec);
  check(BSDict,   BSDict);
  check(BCDict,   BDict);
  check(BSKeyset, BSKeyset);
  check(BCKeyset, BKeyset);
  return t;
}

Type loosen_all(Type t) {
  return loosen_dvarrayness(
    loosen_staticness(
      loosen_emptiness(
        loosen_values(std::move(t))
      )
    )
  );
}

Type add_nonemptiness(Type t) {
  auto const check = [&](trep a, trep b){
    if (t.m_bits & a) t.m_bits = static_cast<trep>(t.m_bits | b);
  };
  check(BSPArrE,   BSPArrN);
  check(BCPArrE,   BPArrN);
  check(BSVArrE,   BSVArrN);
  check(BCVArrE,   BVArrN);
  check(BSDArrE,   BSDArrN);
  check(BCDArrE,   BDArrN);
  check(BSVecE,    BSVecN);
  check(BCVecE,    BVecN);
  check(BSDictE,   BSDictN);
  check(BCDictE,   BDictN);
  check(BSKeysetE, BSKeysetN);
  check(BCKeysetE, BKeysetN);
  return t;
}

Type remove_uninit(Type t) {
  assert(t.subtypeOf(TCell));
  if (!t.couldBe(TUninit))  return t;
  if (t.subtypeOf(TUninit)) return TBottom;
  if (t.subtypeOf(TNull))   return TInitNull;
  if (t.subtypeOf(TPrim))   return TInitPrim;
  if (t.subtypeOf(TUnc))    return TInitUnc;
  return TInitCell;
}

Type assert_emptiness(Type t) {
  if (t.subtypeOfAny(TTrue, TArrN, TVecN, TDictN, TKeysetN)) {
    return TBottom;
  }
  if (!could_have_magic_bool_conversion(t) && t.subtypeOf(TOptObj)) {
    return TInitNull;
  }

  auto remove = [&] (trep m, trep e) {
    if ((t.m_bits & m) == t.m_bits) {
      auto bits = static_cast<trep>(t.m_bits & e);
      if (t.hasData() && !mayHaveData(bits)) {
        t = Type { bits };
      } else {
        t.m_bits = bits;
      }
      return true;
    }
    return false;
  };

  if (remove(BOptArr, BOptArrE) || remove(BOptVec, BOptVecE) ||
      remove(BOptDict, BOptDictE) || remove(BOptKeyset, BOptKeysetE)) {
    return t;
  }

  if (t.subtypeOf(TInt))     return ival(0);
  if (t.subtypeOf(TBool))    return TFalse;
  if (t.subtypeOf(TDbl))     return dval(0);
  if (t.subtypeOf(TSStr))    return sempty();

  if (t.subtypeOf(TOptInt))  return opt(ival(0));
  if (t.subtypeOf(TOptBool)) return opt(TFalse);
  if (t.subtypeOf(TOptDbl))  return opt(dval(0));
  if (t.subtypeOf(TOptSStr)) return opt(sempty());

  return t;
}

Type assert_nonemptiness(Type t) {
  if (is_opt(t)) t = unopt(std::move(t));
  if (t.subtypeOfAny(TNull, TFalse, TArrE, TVecE, TDictE, TKeysetE)) {
    return TBottom;
  }
  if (t.subtypeOf(TBool)) return TTrue;

  auto remove = [&] (trep m, trep e) {
    if ((t.m_bits & m) == t.m_bits) {
      t.m_bits = static_cast<trep>(t.m_bits & e);
      return true;
    }
    return false;
  };

  if (remove(BOptArr, BOptArrN) || remove(BOptVec, BOptVecN) ||
      remove(BOptDict, BOptDictN) || remove(BOptKeyset, BOptKeysetN)) {
    return t;
  }

  return remove_uninit(std::move(t));
}

//////////////////////////////////////////////////////////////////////

/*
 * For known strings that are strictly integers, we'll set both the known
 * integer and string keys, so generally the int case should be checked first
 * below.
 *
 * For keys that could be strings, we have to assume they could be
 * strictly-integer strings. After disection, the effective type we can assume
 * for the array key is in `type'. If the key might coerce to an integer, TInt
 * will be unioned into `type'. So, if `type' is TStr, its safe to assume it
 * will not coerce.
 *
 * `mayThrow' will be set if the key coercion could possibly throw.
 *
 * If the key might be strange (array or object), `type' will be unchanged so it
 * can be detected later on.
 */

ArrKey disect_array_key(const Type& keyTy) {
  auto ret = ArrKey{};

  if (keyTy.subtypeOf(TOptInt)) {
    if (keyTy.subtypeOf(TInt)) {
      if (keyTy.strictSubtypeOf(TInt)) {
        ret.i = keyTy.m_data.ival;
        ret.type = ival(*ret.i);
        return ret;
      }
      ret.type = keyTy;
      return ret;
    }
    // The key could be an integer or a null, which means it might become the
    // empty string. Either way, its an uncounted value.
    ret.type = TUncArrKey;
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }

  if (keyTy.subtypeOf(TOptStr)) {
    if (keyTy.subtypeOf(TStr)) {
      if (keyTy.strictSubtypeOf(TStr) && keyTy.m_dataTag == DataTag::Str) {
        int64_t i;
        if (keyTy.m_data.sval->isStrictlyInteger(i)) {
          ret.i = i;
          ret.type = ival(i);
          ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
        } else {
          ret.s = keyTy.m_data.sval;
          ret.type = keyTy;
        }
        return ret;
      }
      // Might stay a string or become an integer. The effective type is
      // uncounted if the string is static.
      ret.type = keyTy.subtypeOf(TSStr) ? TUncArrKey : TArrKey;
      ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
      return ret;
    }
    // If we have an OptStr with a value, we can at least exclude the
    // possibility of integer-like strings by looking at that value.
    // But we can't use the value itself, because if it is null the key
    // will act like the empty string.  In that case, the code uses the
    // static empty string, so if it was an OptCStr it needs to
    // incorporate SStr, but an OptSStr can stay as SStr.
    if (keyTy.strictSubtypeOf(TOptStr) && keyTy.m_dataTag == DataTag::Str) {
      int64_t ignore;
      if (!keyTy.m_data.sval->isStrictlyInteger(ignore)) {
        ret.type = keyTy.strictSubtypeOf(TOptSStr) ? TSStr : TStr;
        ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
        return ret;
      }
    }
    // An optional string is fine because a null will just become the empty
    // string. So, if the string is static, the effective type is uncounted
    // still. The effective type is ArrKey because it might become an integer.
    ret.type = keyTy.subtypeOf(TOptSStr) ? TUncArrKey : TArrKey;
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }

  if (keyTy.subtypeOf(TOptArrKey)) {
    // The key is an integer, string, or null. The effective type is int or
    // string because null will become the empty string.
    ret.type = is_opt(keyTy) ? unopt(keyTy) : keyTy;
    return ret;
  }

  if (keyTy.strictSubtypeOf(TDbl)) {
    ret.i = double_to_int64(keyTy.m_data.dval);
    ret.type = ival(*ret.i);
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TNum)) {
    ret.type = TInt;
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TNull)) {
    ret.s = s_empty.get();
    ret.type = sempty();
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TRes)) {
    ret.type = TInt;
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TTrue)) {
    ret.i = 1;
    ret.type = ival(1);
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TFalse)) {
    ret.i = 0;
    ret.type = ival(0);
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TBool)) {
    ret.type = TInt;
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }
  if (keyTy.subtypeOf(TPrim)) {
    ret.type = TUncArrKey;
    ret.mayThrow = RuntimeOption::EvalHackArrCompatNotices;
    return ret;
  }

  // The key could be something strange like an array or an object. This can
  // raise warnings, so always assume it may throw. Keep the type as-is so that
  // we can detect this case at the point of the set.

  if (!keyTy.subtypeOf(TInitCell)) {
    ret.type = TInitCell;
    ret.mayThrow = true;
    return ret;
  }

  ret.type = keyTy;
  ret.mayThrow = true;
  return ret;
}

/*
 * Extract map[key] when map is known to have DataTag::ArrLikeVal.
 * Returns a std::pair where the first is the Type of the result, and
 * the second is a flag; if the flag is true, the element definitely
 * existed in the array.
 */
std::pair<Type,bool> arr_val_elem(const Type& aval, const ArrKey& key) {
  assert(aval.m_dataTag == DataTag::ArrLikeVal);
  auto ad = aval.m_data.aval;
  auto const isPhpArray = aval.subtypeOf(TOptArr);
  if (key.i) {
    if (auto const r = ad->rval(*key.i)) {
      return { from_cell(r.tv()), true };
    }
    return { isPhpArray ? TInitNull : TBottom, false };
  } else if (key.s) {
    if (auto const r = ad->rval(*key.s)) {
      return { from_cell(r.tv()), true };
    }
    return { isPhpArray ? TInitNull : TBottom, false };
  }

  auto const couldBeInt = key.type.couldBe(TInt);
  auto const couldBeStr = key.type.couldBe(TStr);
  auto ty = isPhpArray ? TInitNull : TBottom;
  IterateKV(ad, [&] (Cell k, TypedValue v) {
      if (isStringType(k.m_type) ? couldBeStr : couldBeInt) {
        ty |= from_cell(v);
        return TInitCell.subtypeOf(ty);
      }
      return false;
    });
  return { ty, false };
}

/*
 * Extract map[key] when map is known to have DataTag::ArrLikeMap
 * returns a std::pair where the first is the Type of the result, and
 * the second is a flag; if the flag is true, the element definitely
 * existed in the array.
 */
std::pair<Type,bool> arr_map_elem(const Type& map, const ArrKey& key) {
  assert(map.m_dataTag == DataTag::ArrLikeMap);
  auto const isPhpArray = map.subtypeOf(TOptArr);
  if (auto const k = key.tv()) {
    auto r = map.m_data.map->map.find(*k);
    if (r != map.m_data.map->map.end()) return { r->second, true };
    return { isPhpArray ? TInitNull : TBottom, false };
  }
  auto couldBeInt = key.type.couldBe(TInt);
  auto couldBeStr = key.type.couldBe(TStr);
  auto ty = isPhpArray ? TInitNull : TBottom;
  for (auto const& kv : map.m_data.map->map) {
    if (isStringType(kv.first.m_type) ? couldBeStr : couldBeInt) {
      ty |= kv.second;
      if (TInitCell.subtypeOf(ty)) break;
    }
  }

  return { ty, false };
}

/*
 * Extract pack[key] when pack is known to have DataTag::ArrLikePacked
 * returns a std::pair where the first is the Type of the result, and
 * the second is a flag; if the flag is true, the element definitely
 * existed in the array.
 */
std::pair<Type,bool> arr_packed_elem(const Type& pack, const ArrKey& key) {
  assert(pack.m_dataTag == DataTag::ArrLikePacked);
  auto const isPhpArray = pack.subtypeOf(TOptArr);
  if (key.i) {
    if (*key.i >= 0 && *key.i < pack.m_data.packed->elems.size()) {
      return { pack.m_data.packed->elems[*key.i], true };
    }
    return { isPhpArray ? TInitNull : TBottom, false };
  } else if (!key.type.couldBe(TInt)) {
    return { isPhpArray ? TInitNull : TBottom, false };
  }
  auto ret = packed_values(*pack.m_data.packed);
  if (isPhpArray) {
    ret |= TInitNull;
  }
  return { ret, false };
}

/*
 * Extract pack[key] when pack is known to have DataTag::ArrLikePackedN
 */
std::pair<Type,bool> arr_packedn_elem(const Type& pack, const ArrKey& key) {
  assert(pack.m_dataTag == DataTag::ArrLikePackedN);
  auto const isPhpArray = (pack.m_bits & BOptArr) == pack.m_bits;
  if (key.s || !key.type.couldBe(TInt) || (key.i && *key.i < 0)) {
    return {isPhpArray ? TInitNull : TBottom, false};
  }

  if (isPhpArray) {
    return {union_of(pack.m_data.packedn->type, TInitNull), false};
  }

  return {pack.m_data.packedn->type, false};
}

/*
 * Apply the effects of pack[key] = val, when pack is known to have
 * DataTag::ArrLikePackedN.
 *
 * Returns true iff the key is known to be in range.
 */
bool arr_packedn_set(Type& pack,
                     const ArrKey& key,
                     const Type& val,
                     bool maybeEmpty) {
  assert(pack.m_dataTag == DataTag::ArrLikePackedN);
  assert(key.type.subtypeOf(TArrKey));

  auto const isPhpArray = (pack.m_bits & BOptArr) == pack.m_bits;
  auto const isVecArray = (pack.m_bits & BOptVec) == pack.m_bits;
  auto& ty = pack.m_data.packedn.mutate()->type;
  ty |= val;

  if (key.i) {
    // If the key is known to be in range - its still a packedn
    if (isPhpArray) {
      if (!*key.i) return true;
      if (!maybeEmpty && *key.i == 1) return true;
    } else if (!maybeEmpty && !*key.i) {
      return true;
    }
    pack.m_bits = (*key.i < 0)
      ? promote_varray(pack.m_bits)
      : maybe_promote_varray(pack.m_bits);
  } else {
    pack.m_bits = key.type.subtypeOf(TStr)
      ? promote_varray(pack.m_bits)
      : maybe_promote_varray(pack.m_bits);
  }

  if (!isVecArray) {
    pack = mapn_impl(pack.m_bits, union_of(TInt, key.type), std::move(ty));
  }
  return false;
}

/*
 * Apply the effects of map[key] = val, when map is known to have
 * DataTag::ArrLikeMap.
 *
 * Returns true iff we hit a specific element (this matters for
 * hack-array types, since they may throw for other cases).
 */
bool arr_map_set(Type& map,
                 const ArrKey& key,
                 const Type& val) {
  assert(map.m_dataTag == DataTag::ArrLikeMap);
  assert(key.type.subtypeOf(TArrKey));
  assert(!map.subtypeOf(TVArr));

  if (auto const k = key.tv()) {
    auto r = map.m_data.map.mutate()->map.emplace_back(*k, val);
    // if the element existed, and was a ref, its still a ref after
    // assigning to it
    if (!r.second && r.first->second.subtypeOf(TInitCell)) {
      map.m_data.map.mutate()->map.update(r.first, val);
    }
    return true;
  }
  auto mkv = map_key_values(*map.m_data.map);
  map = mapn_impl(map.m_bits,
                  union_of(std::move(mkv.first), key.type),
                  union_of(std::move(mkv.second), val));
  return true;
}

/*
 * Apply the effects of pack[key] = val, when pack is known to have
 * DataTag::ArrLikePacked.
 *
 * Returns true iff the key is known to be in range.
 */
bool arr_packed_set(Type& pack,
                    const ArrKey& key,
                    const Type& val) {
  assert(pack.m_dataTag == DataTag::ArrLikePacked);
  assert(key.type.subtypeOf(TArrKey));

  auto const isVecArray = pack.subtypeOf(TOptVec);
  if (key.i) {
    if (*key.i >= 0) {
      if (*key.i < pack.m_data.packed->elems.size()) {
        auto& current = pack.m_data.packed.mutate()->elems[*key.i];
        // if the element was a ref, its still a ref after assigning to it
        if (current.subtypeOf(TInitCell)) {
          current = val;
        }
        return true;
      }
      if (!isVecArray && *key.i == pack.m_data.packed->elems.size()) {
        pack.m_data.packed.mutate()->elems.push_back(val);
        return true;
      }
    }
    if (isVecArray) {
      pack = TBottom;
      return false;
    }
    pack.m_bits = promote_varray(pack.m_bits);
  } else {
    pack.m_bits = key.type.subtypeOf(TStr)
      ? promote_varray(pack.m_bits)
      : maybe_promote_varray(pack.m_bits);
  }

  if (!isVecArray) {
    if (auto const v = key.tv()) {
      MapElems elems;
      auto idx = int64_t{0};
      for (auto const& t : pack.m_data.packed->elems) {
        elems.emplace_back(make_tv<KindOfInt64>(idx++), t);
      }
      elems.emplace_back(*v, val);
      pack = map_impl(pack.m_bits, std::move(elems));
      return true;
    }

    auto ty = union_of(packed_values(*pack.m_data.packed), val);
    pack = mapn_impl(pack.m_bits, union_of(TInt, key.type), std::move(ty));
    return false;
  }

  auto ty = union_of(packed_values(*pack.m_data.packed), val);
  pack = packedn_impl(pack.m_bits, std::move(ty));

  return false;
}

bool arr_mapn_set(Type& map,
                  const ArrKey& key,
                  const Type& val) {
  assert(map.m_dataTag == DataTag::ArrLikeMapN);
  assert(key.type.subtypeOf(TArrKey));
  assert(!map.subtypeOf(TVArr));
  auto& mapn = *map.m_data.mapn.mutate();
  mapn.val |= val;
  mapn.key |= key.type;
  assert(map.checkInvariants());
  return true;
}

Type arr_map_newelem(Type& map, const Type& val) {
  assert(map.m_dataTag == DataTag::ArrLikeMap);
  int64_t lastK = -1;
  for (auto const& kv : map.m_data.map->map) {
    if (kv.first.m_type == KindOfInt64 &&
        kv.first.m_data.num > lastK) {
      lastK = kv.first.m_data.num;
    }
  }

  if (lastK == std::numeric_limits<int64_t>::max()) {
    return TInt;
  }
  map.m_data.map.mutate()->map.emplace_back(make_tv<KindOfInt64>(lastK + 1),
                                            val);
  return ival(lastK + 1);
}

std::pair<Type, ThrowMode> array_like_elem(const Type& arr, const ArrKey& key) {
  const bool maybeEmpty = arr.m_bits & BArrLikeE;
  const bool mustBeStatic = (arr.m_bits & BSArrLike) == arr.m_bits;

  auto const isPhpArray = arr.subtypeOf(TOptArr);
  if (!(arr.m_bits & BArrLikeN)) {
    assert(maybeEmpty);
    return { isPhpArray ? TInitNull : TBottom, ThrowMode::MissingElement };
  }
  auto pair = [&]() -> std::pair<Type, bool> {
    switch (arr.m_dataTag) {
    case DataTag::Str:
    case DataTag::Obj:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Cls:
    case DataTag::RefInner:
      not_reached();

    case DataTag::None:
      return { mustBeStatic ? TInitUnc : TInitCell, false };

    case DataTag::ArrLikeVal:
      return arr_val_elem(arr, key);

    case DataTag::ArrLikePacked:
      return arr_packed_elem(arr, key);

    case DataTag::ArrLikePackedN:
      return arr_packedn_elem(arr, key);

    case DataTag::ArrLikeMap:
      return arr_map_elem(arr, key);

    case DataTag::ArrLikeMapN:
      if (isPhpArray) {
        return { union_of(arr.m_data.mapn->val, TInitNull), false};
      } else {
        return { arr.m_data.mapn->val, false };
      }
    }
    not_reached();
  }();

  std::pair<Type, ThrowMode> ret = {
    std::move(pair.first),
    key.mayThrow ? ThrowMode::MaybeBadKey :
    pair.second ? ThrowMode::None : ThrowMode::MaybeMissingElement
  };

  if (!ret.first.subtypeOf(TInitCell)) {
    ret.first = TInitCell;
  }

  if (maybeEmpty) {
    if (isPhpArray) ret.first |= TInitNull;
    if (ret.second == ThrowMode::None) {
      ret.second = ThrowMode::MaybeMissingElement;
    }
  }

  return ret;
}

std::pair<Type,ThrowMode>
array_elem(const Type& arr, const Type& undisectedKey) {
  assert(arr.subtypeOf(TArr));
  auto const key = disect_array_key(undisectedKey);
  return array_like_elem(arr, key);
}

/*
 * Note: for now we're merging counted arrays into whatever type it used to
 * have in the following set functions, and returning arr_*'s in some cases,
 * because it might become counted.
 *
 * To be able to assume it is actually counted it if used to be
 * static, we need to add code checking for keys that are one of the
 * "illegal offset type" of keys; in addition, even if we know *now*
 * that its counted, after further optimizations we might be able to
 * fully determine its contents, and replace it with a static array.
 *
 * A similar issue applies if you want to take out emptiness when a set occurs.
 * If the key could be an illegal key type, the array may remain empty.
 */

std::pair<Type,ThrowMode> array_like_set(Type arr,
                                         const ArrKey& key,
                                         const Type& valIn) {
  const bool maybeEmpty = arr.m_bits & BArrLikeE;
  const bool isVector   = arr.m_bits & BOptVec;
  const bool isPhpArray = arr.m_bits & BOptArr;
  DEBUG_ONLY const bool isVArray   = (arr.m_bits & BOptVArr) == arr.m_bits;
  const bool validKey   = key.type.subtypeOf(isVector ? TInt : TArrKey);

  trep bits = combine_dv_arr_like_bits(arr.m_bits, BArrLikeN);
  if (validKey) bits = static_cast<trep>(bits & ~BArrLikeE);

  auto const fixRef  = !isPhpArray && valIn.couldBe(TRef);
  auto const throwMode = !fixRef && validKey && !key.mayThrow ?
    ThrowMode::None : ThrowMode::BadOperation;
  auto const& val    = fixRef ? TInitCell : valIn;
  // We don't want to store types more general than TArrKey into specialized
  // array type keys. If the key was strange (array or object), it will be more
  // general than TArrKey (this is needed so we can set validKey above), so
  // force it to TArrKey.
  auto const& fixedKey = validKey
    ? key
    : []{ ArrKey key; key.type = TArrKey; key.mayThrow = true; return key; }();

  if (!(arr.m_bits & BArrLikeN)) {
    assert(maybeEmpty);
    if (isVector) return { TBottom, ThrowMode::BadOperation };
    if (fixedKey.i) {
      if (!*fixedKey.i) {
        return { packed_impl(bits, { val }), throwMode };
      }
      bits = promote_varray(bits);
    } else {
      bits = fixedKey.type.subtypeOf(TStr)
        ? promote_varray(bits)
        : maybe_promote_varray(bits);
    }
    if (auto const k = fixedKey.tv()) {
      MapElems m;
      m.emplace_back(*k, val);
      return { map_impl(bits, std::move(m)), throwMode };
    }
    return { mapn_impl(bits, fixedKey.type, val), throwMode };
  }

  auto emptyHelper = [&] (const Type& inKey,
                          const Type& inVal) -> std::pair<Type,ThrowMode> {
    bits = fixedKey.type.subtypeOf(TStr)
      ? promote_varray(bits)
      : maybe_promote_varray(bits);
    return { mapn_impl(bits,
                       union_of(inKey, fixedKey.type),
                       union_of(inVal, val)), throwMode };
  };

  arr.m_bits = bits;

  switch (arr.m_dataTag) {
  case DataTag::Str:
  case DataTag::Obj:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
  case DataTag::RefInner:
    not_reached();

  case DataTag::None:
    arr.m_bits = fixedKey.type.subtypeOf(TStr)
      ? promote_varray(arr.m_bits)
      : maybe_promote_varray(arr.m_bits);
    return { std::move(arr), ThrowMode::BadOperation };

  case DataTag::ArrLikeVal:
    if (maybeEmpty && !isVector) {
      auto kv = val_key_values(arr.m_data.aval);
      return emptyHelper(kv.first, kv.second);
    } else {
      if (auto d = toDArrLikePacked(arr.m_data.aval)) {
        return array_like_set(packed_impl(bits, std::move(d->elems)),
                              key, valIn);
      }
      assert(!isVector);
      assert(!isVArray);
      // We know its not packed, so this should always succeed
      auto d = toDArrLikeMap(arr.m_data.aval);
      return array_like_set(map_impl(bits, std::move(d->map)),
                            key, valIn);
    }

  case DataTag::ArrLikePacked:
    // Setting element zero of a maybe empty, 1 element packed array
    // turns it into a 1 element packed array.
    if (maybeEmpty && !isVector &&
        (!fixedKey.i ||
         *fixedKey.i ||
         arr.m_data.packed->elems.size() != 1)) {
      return emptyHelper(TInt, packed_values(*arr.m_data.packed));
    } else {
      auto const inRange = arr_packed_set(arr, fixedKey, val);
      return { std::move(arr), inRange ? throwMode : ThrowMode::BadOperation };
    }

  case DataTag::ArrLikePackedN:
    if (maybeEmpty && !isVector) {
      return emptyHelper(TInt, arr.m_data.packedn->type);
    } else {
      auto const inRange = arr_packedn_set(arr, fixedKey, val, false);
      return { std::move(arr), inRange ? throwMode : ThrowMode::BadOperation };
    }

  case DataTag::ArrLikeMap:
    assert(!isVector);
    assert(!isVArray);
    if (maybeEmpty) {
      auto mkv = map_key_values(*arr.m_data.map);
      return emptyHelper(std::move(mkv.first), std::move(mkv.second));
    } else {
      auto const inRange = arr_map_set(arr, fixedKey, val);
      return { std::move(arr), inRange ? throwMode : ThrowMode::BadOperation };
    }

  case DataTag::ArrLikeMapN:
    assert(!isVector);
    assert(!isVArray);
    if (maybeEmpty) {
      return emptyHelper(arr.m_data.mapn->key, arr.m_data.mapn->val);
    } else {
      auto const inRange = arr_mapn_set(arr, fixedKey, val);
      return { std::move(arr), inRange ? throwMode : ThrowMode::BadOperation };
    }
  }

  not_reached();
}

std::pair<Type, ThrowMode> array_set(Type arr,
                                     const Type& undisectedKey,
                                     const Type& val) {
  assert(arr.subtypeOf(TArr));

  // Unless you know an array can't cow, you don't know if the TRef
  // will stay a TRef or turn back into a TInitCell.  Generally you
  // want a TInitGen.
  always_assert((val == TBottom || !val.subtypeOf(TRef)) &&
                "You probably don't want to put Ref types into arrays ...");

  auto const key = disect_array_key(undisectedKey);
  assert(key.type != TBottom);
  return array_like_set(std::move(arr), key, val);
}

std::pair<Type,Type> array_like_newelem(Type arr, const Type& val) {

  if (arr.m_bits & BOptKeyset) {
    auto const key = disect_strict_key(val);
    if (key.type == TBottom) return { TBottom, TInitCell };
    return { array_like_set(std::move(arr), key, key.type).first, val };
  }

  const bool maybeEmpty = arr.m_bits & BArrLikeE;
  const bool isVector = arr.m_bits & BOptVec;
  const bool isVArray = (arr.m_bits & BOptVArr) == arr.m_bits;

  trep bits = combine_dv_arr_like_bits(arr.m_bits, BArrLikeN);
  bits = static_cast<trep>(bits & ~BArrLikeE);

  if (!(arr.m_bits & BArrLikeN)) {
    assert(maybeEmpty);
    return { packed_impl(bits, { val }), ival(0) };
  }

  auto emptyHelper = [&] (const Type& inKey,
                          const Type& inVal) -> std::pair<Type,Type> {
    if (isVector || isVArray) {
      assert(inKey.subtypeOf(TInt));
      return { packedn_impl(bits, union_of(inVal, val)), TInt };
    }

    return { mapn_impl(bits,
                       union_of(inKey, TInt),
                       union_of(inVal, val)), TInt };
  };

  switch (arr.m_dataTag) {
  case DataTag::Str:
  case DataTag::Obj:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
  case DataTag::RefInner:
    not_reached();

  case DataTag::None:
    arr.m_bits = bits;
    return { std::move(arr), TInt };

  case DataTag::ArrLikeVal:
    if (maybeEmpty) {
      auto kv = val_key_values(arr.m_data.aval);
      return emptyHelper(kv.first, kv.second);
    } else {
      if (auto d = toDArrLikePacked(arr.m_data.aval)) {
        return array_like_newelem(
          packed_impl(bits, std::move(d->elems)), val);
      }
      assert(!isVector);
      // We know its not packed, so this should always succeed.
      auto d = toDArrLikeMap(arr.m_data.aval);
      return array_like_newelem(map_impl(bits, std::move(d->map)), val);
    }

  case DataTag::ArrLikePacked:
    if (maybeEmpty) {
      return emptyHelper(TInt, packed_values(*arr.m_data.packed));
    } else {
      arr.m_bits = bits;
      auto len = arr.m_data.packed->elems.size();
      arr.m_data.packed.mutate()->elems.push_back(val);
      return { std::move(arr), ival(len) };
    }

  case DataTag::ArrLikePackedN:
    if (maybeEmpty) {
      return emptyHelper(TInt, arr.m_data.packedn->type);
    } else {
      arr.m_bits = bits;
      auto packedn = arr.m_data.packedn.mutate();
      packedn->type |= val;
      return { std::move(arr), TInt };
    }

  case DataTag::ArrLikeMap:
    assert(!isVector);
    assert(!isVArray);
    if (maybeEmpty) {
      auto mkv = map_key_values(*arr.m_data.map);
      return emptyHelper(mkv.first, mkv.second);
    } else {
      arr.m_bits = bits;
      auto const idx = arr_map_newelem(arr, val);
      return { std::move(arr), idx };
    }

  case DataTag::ArrLikeMapN:
    assert(!isVector);
    assert(!isVArray);
    if (maybeEmpty) {
      return emptyHelper(arr.m_data.mapn->key, arr.m_data.mapn->val);
    }
    return { mapn_impl(bits,
                       union_of(arr.m_data.mapn->key, TInt),
                       union_of(arr.m_data.mapn->val, val)),
             TInt };
  }

  not_reached();
}

std::pair<Type,Type> array_newelem(Type arr, const Type& val) {
  assert(arr.subtypeOf(TArr));

  // Unless you know an array can't cow, you don't know if the TRef
  // will stay a TRef or turn back into a TInitCell.  Generally you
  // want a TInitGen.
  always_assert((val == TBottom || !val.subtypeOf(TRef)) &&
         "You probably don't want to put Ref types into arrays ...");

  return array_like_newelem(std::move(arr), val);
}

IterTypes iter_types(const Type& iterable) {
  // Only array types and objects can be iterated. Everything else raises a
  // warning and jumps out of the loop.
  if (!iterable.couldBeAny(TArr, TVec, TDict, TKeyset, TObj)) {
    return { TBottom, TBottom, IterTypes::Count::Empty, true, true };
  }

  // Optional types are okay here because a null will not set any locals (but it
  // might throw).
  if (!iterable.subtypeOfAny(TOptArr, TOptVec, TOptDict, TOptKeyset)) {
    return {
      TInitCell,
      TInitCell,
      IterTypes::Count::Any,
      true,
      iterable.couldBe(TObj)
    };
  }

  auto const mayThrow = is_opt(iterable);

  if (iterable.subtypeOfAny(TOptArrE, TOptVecE, TOptDictE, TOptKeysetE)) {
    return { TBottom, TBottom, IterTypes::Count::Empty, mayThrow, false };
  }

  // If we get a null, it will be as if we have any empty array, so consider
  // that possibly "empty".
  auto const maybeEmpty =
    mayThrow ||
    !iterable.subtypeOfAny(TOptArrN, TOptVecN, TOptDictN, TOptKeysetN);

   auto const count = [&](folly::Optional<size_t> size){
    if (size) {
      assert(*size > 0);
      if (*size == 1) {
        return maybeEmpty
          ? IterTypes::Count::ZeroOrOne
          : IterTypes::Count::Single;
      }
    }
    return maybeEmpty ? IterTypes::Count::Any : IterTypes::Count::NonEmpty;
  };

  if (!is_specialized_array_like(iterable)) {
    auto kv = [&]() -> std::pair<Type, Type> {
      if (iterable.subtypeOf(TOptSVec))    return { TInt, TInitUnc };
      if (iterable.subtypeOf(TOptSDict))   return { TUncArrKey, TInitUnc };
      if (iterable.subtypeOf(TOptSKeyset)) return { TUncArrKey, TUncArrKey };
      if (iterable.subtypeOf(TOptSVArr))   return { TInt, TInitUnc };
      if (iterable.subtypeOf(TOptSArr))    return { TUncArrKey, TInitUnc };

      if (iterable.subtypeOf(TOptVec))     return { TInt, TInitCell };
      if (iterable.subtypeOf(TOptDict))    return { TArrKey, TInitCell };
      if (iterable.subtypeOf(TOptKeyset))  return { TArrKey, TArrKey };
      if (iterable.subtypeOf(TOptVArr))    return { TInt, TInitCell };
      if (iterable.subtypeOf(TOptArr))     return { TArrKey, TInitCell };

      always_assert(false);
    }();

    return {
      std::move(kv.first),
      std::move(kv.second),
      count(folly::none),
      mayThrow,
      false
    };
  }

  switch (iterable.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::Obj:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
  case DataTag::RefInner:
    always_assert(0);
  case DataTag::ArrLikeVal: {
    auto kv = val_key_values(iterable.m_data.aval);
    return {
      std::move(kv.first),
      std::move(kv.second),
      count(iterable.m_data.aval->size()),
      mayThrow,
      false
    };
  }
  case DataTag::ArrLikePacked:
    return {
      TInt,
      packed_values(*iterable.m_data.packed),
      count(iterable.m_data.packed->elems.size()),
      mayThrow,
      false
    };
  case DataTag::ArrLikePackedN:
    return {
      TInt,
      iterable.m_data.packedn->type,
      count(folly::none),
      mayThrow,
      false
    };
  case DataTag::ArrLikeMap: {
    auto kv = map_key_values(*iterable.m_data.map);
    return {
      std::move(kv.first),
      std::move(kv.second),
      count(iterable.m_data.map->map.size()),
      mayThrow,
      false
    };
  }
  case DataTag::ArrLikeMapN:
    return {
      iterable.m_data.mapn->key,
      iterable.m_data.mapn->val,
      count(folly::none),
      mayThrow,
      false
    };
  }

  not_reached();
}

bool could_run_destructor(const Type& t) {
  if (t.couldBe(TObj)) return true;

  auto const couldBeArrWithDestructors =
    t.m_bits & (BCArrN | BCVecN | BCDictN);

  if (t.couldBe(TRef)) {
    if (!couldBeArrWithDestructors && is_ref_with_inner(t)) {
      return could_run_destructor(*t.m_data.inner);
    }
    return true;
  }

  if (!couldBeArrWithDestructors) return false;

  switch (t.m_dataTag) {
  case DataTag::None:
  case DataTag::Str:
  case DataTag::Obj:
  case DataTag::Int:
  case DataTag::Dbl:
  case DataTag::Cls:
  case DataTag::RefInner:
    return true;
  case DataTag::ArrLikeVal: return false;
  case DataTag::ArrLikePacked:
    for (auto const& e : t.m_data.packed->elems) {
      if (could_run_destructor(e)) return true;
    }
    return false;
  case DataTag::ArrLikePackedN:
    return could_run_destructor(t.m_data.packedn->type);
  case DataTag::ArrLikeMap:
    for (auto const& kv : t.m_data.map->map) {
      if (could_run_destructor(kv.second)) return true;
    }
    return false;
  case DataTag::ArrLikeMapN:
    return could_run_destructor(t.m_data.mapn->val);
  }

  not_reached();
}

bool could_copy_on_write(const Type& t) {
  return t.m_bits & (BCStr | BCArrN | BCVecN | BCDictN | BCKeysetN);
}

//////////////////////////////////////////////////////////////////////

ArrKey disect_vec_key(const Type& keyTy) {
  auto ret = ArrKey{};

  if (!keyTy.couldBe(TInt)) {
    ret.type = TBottom;
    ret.mayThrow = true;
    return ret;
  }

  // If the key is null, we'll throw, so we can assume its not for the effective
  // type (and mark it as potentially throwing). We check for this explicitly
  // here rather than falling through so we can take advantage of something like
  // ?Int=123.
  if (keyTy.subtypeOf(TOptInt)) {
    if (keyTy.m_dataTag == DataTag::Int) {
      ret.i = keyTy.m_data.ival;
      ret.type = ival(*ret.i);
    } else {
      ret.type = TInt;
    }
    ret.mayThrow = !keyTy.subtypeOf(TInt);
    return ret;
  }

  // Something else. Same reasoning as above. We can assume its an TInt because
  // it will throw otherwise.
  ret.type = TInt;
  ret.mayThrow = true;
  return ret;
}

std::pair<Type, ThrowMode>
vec_elem(const Type& vec, const Type& undisectedKey) {
  auto const key = disect_vec_key(undisectedKey);
  if (key.type == TBottom) return {TBottom, ThrowMode::BadOperation};
  return array_like_elem(vec, key);
}

std::pair<Type, ThrowMode>
vec_set(Type vec, const Type& undisectedKey, const Type& val) {
  if (!val.couldBe(TInitCell)) return {TBottom, ThrowMode::BadOperation};

  auto const key = disect_vec_key(undisectedKey);
  if (key.type == TBottom) return {TBottom, ThrowMode::BadOperation};

  return array_like_set(std::move(vec), key, val);
}

std::pair<Type,Type> vec_newelem(Type vec, const Type& val) {
  return array_like_newelem(std::move(vec),
                            val.subtypeOf(TInitCell) ? val : TInitCell);
}

//////////////////////////////////////////////////////////////////////

ArrKey disect_strict_key(const Type& keyTy) {
  auto ret = ArrKey{};

  if (!keyTy.couldBe(TArrKey)) {
    ret.type = TBottom;
    ret.mayThrow = true;
    return ret;
  }

  // If the key is null, we'll throw, so we can assume its not for the effective
  // type (but mark it as potentially throwing).
  if (keyTy.subtypeOf(TOptArrKey)) {
    if (keyTy.m_dataTag == DataTag::Int) {
      ret.i = keyTy.m_data.ival;
    } else if (keyTy.m_dataTag == DataTag::Str) {
      ret.s = keyTy.m_data.sval;
    }
    ret.type = is_opt(keyTy) ? unopt(keyTy) : keyTy;
    ret.mayThrow = !keyTy.subtypeOf(TArrKey);
    return ret;
  }

  // Something else. Same reasoning as above. We can assume its an TArrKey
  // because it will throw otherwise.
  ret.type = TArrKey;
  ret.mayThrow = true;
  return ret;
}

std::pair<Type, ThrowMode>
dict_elem(const Type& dict, const Type& undisectedKey) {
  auto const key = disect_strict_key(undisectedKey);
  if (key.type == TBottom) return {TBottom, ThrowMode::BadOperation};
  return array_like_elem(dict, key);
}

std::pair<Type, ThrowMode>
dict_set(Type dict, const Type& undisectedKey, const Type& val) {
  if (!val.couldBe(TInitCell)) return {TBottom, ThrowMode::BadOperation};

  auto const key = disect_strict_key(undisectedKey);
  if (key.type == TBottom) return {TBottom, ThrowMode::BadOperation};

  return array_like_set(std::move(dict), key, val);
}

std::pair<Type,Type> dict_newelem(Type dict, const Type& val) {
  return array_like_newelem(std::move(dict),
                            val.subtypeOf(TInitCell) ? val : TInitCell);
}

//////////////////////////////////////////////////////////////////////

std::pair<Type, ThrowMode>
keyset_elem(const Type& keyset, const Type& undisectedKey) {
  auto const key = disect_strict_key(undisectedKey);
  if (key.type == TBottom) return {TBottom, ThrowMode::BadOperation};
  return array_like_elem(keyset, key);
}

std::pair<Type, ThrowMode>
keyset_set(Type /*keyset*/, const Type&, const Type&) {
  // The set operation on keysets is not allowed.
  return {TBottom, ThrowMode::BadOperation};
}

std::pair<Type,Type> keyset_newelem(Type keyset, const Type& val) {
  return array_like_newelem(std::move(keyset), val);
}

//////////////////////////////////////////////////////////////////////

RepoAuthType make_repo_type_arr(ArrayTypeTable::Builder& arrTable,
                                const Type& t) {
  auto const emptiness  = TArrE.couldBe(t) ? RepoAuthType::Array::Empty::Maybe
                                           : RepoAuthType::Array::Empty::No;

  auto const arr = [&]() -> const RepoAuthType::Array* {
    switch (t.m_dataTag) {
    case DataTag::None:
    case DataTag::Str:
    case DataTag::Obj:
    case DataTag::Int:
    case DataTag::Dbl:
    case DataTag::Cls:
    case DataTag::RefInner:
    case DataTag::ArrLikeVal:
    case DataTag::ArrLikeMap:
    case DataTag::ArrLikeMapN:
      return nullptr;
    case DataTag::ArrLikePackedN:
      // TODO(#4205897): we need to use this before it's worth putting
      // in the repo.
      if (false) {
        return arrTable.packedn(
          emptiness,
          make_repo_type(arrTable, t.m_data.packedn->type)
        );
      }
      return nullptr;
    case DataTag::ArrLikePacked:
      {
        std::vector<RepoAuthType> repoTypes;
        std::transform(
          begin(t.m_data.packed->elems), end(t.m_data.packed->elems),
          std::back_inserter(repoTypes),
          [&] (const Type& t2) { return make_repo_type(arrTable, t2); }
        );
        return arrTable.packed(emptiness, repoTypes);
      }
      return nullptr;
    }
    not_reached();
  }();

  auto const tag = [&]() -> RepoAuthType::Tag {
    if (t.subtypeOf(TSVArr))    return RepoAuthType::Tag::SVArr;
    if (t.subtypeOf(TVArr))     return RepoAuthType::Tag::VArr;
    if (t.subtypeOf(TOptSVArr)) return RepoAuthType::Tag::OptSVArr;
    if (t.subtypeOf(TOptVArr))  return RepoAuthType::Tag::OptVArr;
    if (t.subtypeOf(TSDArr))    return RepoAuthType::Tag::SDArr;
    if (t.subtypeOf(TDArr))     return RepoAuthType::Tag::DArr;
    if (t.subtypeOf(TOptSDArr)) return RepoAuthType::Tag::OptSDArr;
    if (t.subtypeOf(TOptDArr))  return RepoAuthType::Tag::OptDArr;
    if (t.subtypeOf(TSArr))     return RepoAuthType::Tag::SArr;
    if (t.subtypeOf(TArr))      return RepoAuthType::Tag::Arr;
    if (t.subtypeOf(TOptSArr))  return RepoAuthType::Tag::OptSArr;
    if (t.subtypeOf(TOptArr))   return RepoAuthType::Tag::OptArr;
    not_reached();
  }();

  return RepoAuthType { tag, arr };
}

RepoAuthType make_repo_type(ArrayTypeTable::Builder& arrTable, const Type& t) {
  assert(!t.couldBe(TCls));
  assert(!t.subtypeOf(TBottom));
  using T = RepoAuthType::Tag;

  if (t.strictSubtypeOf(TObj) || (is_opt(t) && t.strictSubtypeOf(TOptObj))) {
    auto const dobj = dobj_of(t);
    auto const tag =
      is_opt(t)
        ? (dobj.type == DObj::Exact ? T::OptExactObj : T::OptSubObj)
        : (dobj.type == DObj::Exact ? T::ExactObj    : T::SubObj);
    return RepoAuthType { tag, dobj.cls.name() };
  }

  if (t.strictSubtypeOf(TArr) || (is_opt(t) && t.strictSubtypeOf(TOptArr))) {
    return make_repo_type_arr(arrTable, t);
  }

#define X(x) if (t.subtypeOf(T##x)) return RepoAuthType{T::x};
  X(Uninit)
  X(InitNull)
  X(Null)
  X(Int)
  X(OptInt)
  X(Dbl)
  X(OptDbl)
  X(Res)
  X(OptRes)
  X(Bool)
  X(OptBool)
  X(SStr)
  X(OptSStr)
  X(Str)
  X(OptStr)
  X(SVArr)
  X(OptSVArr)
  X(VArr)
  X(OptVArr)
  X(SDArr)
  X(OptSDArr)
  X(DArr)
  X(OptDArr)
  X(SArr)
  X(OptSArr)
  X(Arr)
  X(OptArr)
  X(SVec)
  X(OptSVec)
  X(Vec)
  X(OptVec)
  X(SDict)
  X(OptSDict)
  X(Dict)
  X(OptDict)
  X(SKeyset)
  X(OptSKeyset)
  X(Keyset)
  X(OptKeyset)
  X(Obj)
  X(OptObj)
  X(UncArrKey)
  X(ArrKey)
  X(OptUncArrKey)
  X(OptArrKey)
  X(InitUnc)
  X(Unc)
  X(InitCell)
  X(Cell)
  X(Ref)
  X(InitGen)
  X(Gen)
#undef X
  not_reached();
}

//////////////////////////////////////////////////////////////////////

}}
