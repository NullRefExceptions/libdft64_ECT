#ifndef LIBDFT_TAG_TRAITS_H
#define LIBDFT_TAG_TRAITS_H

#include <string>
#include <set>
#include "ewah.h"
template <typename T>
struct tag_traits
{
};
template <typename T>
T tag_combine(T const &lhs, T const &rhs,uint64_t tid);
template <typename T>
std::string tag_sprint(T const &tag);
template <typename T>
T tag_alloc(unsigned int offset,uint64_t tid);

template <typename T>
inline bool tag_is_empty(T const &tag);

/********************************************************
 uint8_t tags
 ********************************************************/
typedef uint8_t libdft_tag_uint8;

template <>
struct tag_traits<unsigned char>
{
  typedef uint8_t type;
  static const uint8_t cleared_val;
};

template <>
uint8_t tag_combine(uint8_t const &lhs, uint8_t const &rhs,uint64_t tid);
template <>
std::string tag_sprint(uint8_t const &tag);
template <>
uint8_t tag_alloc<uint8_t>(unsigned int offset,uint64_t tid);

template <>
inline bool tag_is_empty(uint8_t const &tag)
{
  return tag == 0;
}

/********************************************************
 uint32_t set tags
 ********************************************************/
typedef std::set<uint32_t> libdft_set_tag;

template<>
struct tag_traits<std::set<uint32_t> >
{
	static const std::set<uint32_t> cleared_val;
};

template<>
std::set<uint32_t> tag_alloc(uint32_t offset,uint64_t tid);

template<>
std::set<uint32_t> tag_combine(std::set<uint32_t> const & lhs, std::set<uint32_t> const & rhs,uint64_t tid);

template<>
std::string tag_sprint(std::set<uint32_t> const & tag);

template <>
inline bool tag_is_empty(std::set<uint32_t> const &tag)
{
  return tag.size() == 0;
}

/********************************************************
ewah tags
********************************************************/
typedef EWAHBoolArray<uint32_t>  libdft_ewah_tag;

template<>
struct tag_traits<EWAHBoolArray<uint32_t>>
{
        typedef EWAHBoolArray<uint32_t> type;
        typedef uint8_t inner_type; // ???
        static const bool is_container = true;
        static const EWAHBoolArray<uint32_t> cleared_val;
        static const EWAHBoolArray<uint32_t> set_val;
};
template<>
EWAHBoolArray<uint32_t> tag_combine(EWAHBoolArray<uint32_t> const & lhs, EWAHBoolArray<uint32_t> const & rhs,uint64_t tid);

template<>
EWAHBoolArray<uint32_t> tag_alloc(unsigned int offset,uint64_t tid);

template<>
std::string tag_sprint(EWAHBoolArray<uint32_t> const & tag);

template <>
inline bool tag_is_empty(EWAHBoolArray<uint32_t> const &tag)
{
  return tag.numberOfOnes() == 0;
}

/********************************************************
bdd tags
********************************************************/
#include "./bdd_tag.h"

typedef lb_type libdft_bdd_tag;

template <>
struct tag_traits<lb_type>
{
  typedef lb_type type;
  static lb_type cleared_val;
};

template <>
lb_type tag_combine(lb_type const &lhs, lb_type const &rhs,uint64_t tid);
// template <> void tag_combine_inplace(lb_type &lhs, lb_type const &rhs);
template <>
std::string tag_sprint(lb_type const &tag);
template <>
lb_type tag_alloc<lb_type>(unsigned int offset,uint64_t tid);

std::vector<tag_seg> tag_get(lb_type);
template <>

inline bool tag_is_empty(lb_type const &tag)
{
  return tag == 0;
}

/********************************************************
ssa  tags
********************************************************/
#include "ssa_tag.h"

typedef ssa_tag libdft_ssa_tag;

template <>
struct tag_traits<ssa_tag>
{
  static ssa_tag cleared_val;
};

template <>
ssa_tag tag_combine(ssa_tag const &lhs, ssa_tag const &rhs,uint64_t tid);
template <>
std::string tag_sprint(ssa_tag const &tag);
template <>
ssa_tag tag_alloc<ssa_tag>(unsigned int offset,uint64_t tid);

inline bool tag_is_empty(ssa_tag const &tag)
{
  return tag == tag_traits<ssa_tag>::cleared_val;
}

/********************************************************
setting
********************************************************/
#if defined(TAG_SSA)
typedef libdft_ssa_tag tag_t;
#elif defined(TAG_BDD)
typedef libdft_bdd_tag tag_t;
#elif defined(TAG_EWAH) 
typedef libdft_ewah_tag tag_t;
#elif defined(TAG_UINT8)
typedef libdft_tag_uint8 tag_t;
#elif defined(TAG_SET)
typedef libdft_set_tag tag_t;
#endif

#endif /* LIBDFT_TAG_TRAITS_H */