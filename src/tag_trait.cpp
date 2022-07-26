#include "pin.H"
#include "tag_traits.h"
#include <string.h>
#include "sylvan.h"
#include "ssa_tag.h"

#ifdef TAINT_PROFILE
uint64_t combine_time=0;
uint64_t alloc_time=0;
uint64_t move_time=0;
#endif

#ifdef TAINT_COUNT
uint64_t combine_count;
uint64_t alloc_count;
uint64_t move_count;
#endif
/********************************************************
 uint8_t tags
 ********************************************************/
template <>
uint8_t tag_combine(uint8_t const &lhs, uint8_t const &rhs,uint64_t tid)
{
  return lhs | rhs;
}

template <>
std::string tag_sprint(uint8_t const &tag)
{
  std::stringstream ss;
  ss << tag;
  return ss.str();
}

template <>
uint8_t tag_alloc<uint8_t>(unsigned int offset,uint64_t tid)
{
  return offset > 0;
}
const uint8_t tag_traits<uint8_t>::cleared_val = 0;

/********************************************************
 uint32_t set tags
 ********************************************************/
const std::set<uint32_t> tag_traits<std::set<uint32_t>>::cleared_val = std::set<uint32_t>();

template <>
std::set<uint32_t> tag_alloc(uint32_t offset,uint64_t tid)
{
	std::set<uint32_t> res;
	res.insert(offset);
	return res;
}

template <>
std::set<uint32_t> tag_combine(std::set<uint32_t> const &lhs, std::set<uint32_t> const &rhs,uint64_t tid)
{
	std::set<uint32_t> res;

	std::set_union(
		lhs.begin(), lhs.end(),
		rhs.begin(), rhs.end(),
		std::inserter(res, res.begin()));

	return res;
}

template <>
std::string tag_sprint(std::set<uint32_t> const &tag)
{
	std::set<uint32_t>::const_iterator t;
	std::stringstream ss;

	ss << "{";
	if (!tag.empty())
	{
		std::set<uint32_t>::const_iterator last = --tag.end(); //std::prev(tag.end());
		for (t = tag.begin(); t != last; t++)
			ss << *t << ", ";
		ss << *(t++);
	}
	ss << "}";
	return ss.str();
}

/********************************************************
ewah tags
********************************************************/
const EWAHBoolArray<uint32_t> tag_traits<EWAHBoolArray<uint32_t>>::cleared_val = EWAHBoolArray<uint32_t>{};
template<>
EWAHBoolArray<uint32_t> tag_combine(EWAHBoolArray<uint32_t> const & lhs, EWAHBoolArray<uint32_t> const & rhs,uint64_t tid) {
#ifdef TAINT_PROFILE
	uint64_t pre = __rdtsc();
#endif
	EWAHBoolArray<uint32_t> result;
	((EWAHBoolArray<uint32_t> &)lhs).logicalor((EWAHBoolArray<uint32_t> &)rhs, result);
#ifdef TAINT_PROFILE
	combine_time+= __rdtsc()- pre;
#endif
	return result;
}

template<>
EWAHBoolArray<uint32_t> tag_alloc(unsigned int offset,uint64_t tid)
{
#ifdef TAINT_PROFILE
	uint64_t pre = __rdtsc();
#endif
	EWAHBoolArray<uint32_t> t;
	t.set(offset);
#ifdef TAINT_PROFILE
	alloc_time+= __rdtsc()- pre;
#endif
	return t;
}

template<>
std::string tag_sprint(EWAHBoolArray<uint32_t> const & tag) {
    std::stringstream ss;
	ss << tag;
    return ss.str();

}

/********************************************************
bdd tags
********************************************************/

BDDTag bdd_tag;
lb_type tag_traits<lb_type>::cleared_val = 0;

template <>
lb_type tag_combine(lb_type const &lhs, lb_type const &rhs,uint64_t tid)
{  	
#ifdef TAINT_PROFILE
	uint64_t pre =  __rdtsc();
	lb_type res = bdd_tag.combine(lhs, rhs);
	combine_time += __rdtsc()-pre;
  	return res;
#else
  return bdd_tag.combine(lhs, rhs);
#endif
}

template <>
std::string tag_sprint(lb_type const &tag)
{
  return bdd_tag.to_string(tag);
}

template <>
lb_type tag_alloc<lb_type>(unsigned int offset,uint64_t tid)
{
#ifdef TAINT_PROFILE
	uint64_t pre =  __rdtsc();
	lb_type res = bdd_tag.insert(offset);
	alloc_time += __rdtsc()-pre;
	return res;
#else
  return bdd_tag.insert(offset);
#endif
}

std::vector<tag_seg> tag_get(lb_type t) { return bdd_tag.find(t); }

/********************************************************
ssa  tags
********************************************************/
ssa_tag tag_traits<ssa_tag>::cleared_val;

template <>
std::string tag_sprint(ssa_tag const &tag)
{
  return ssa_tag_print(tag);
}

template <>
ssa_tag tag_alloc<ssa_tag>(unsigned int offset,uint64_t tid)
{
#ifdef TAINT_PROFILE
  uint64_t pre =  __rdtsc();
  ssa_tag res = ssa_tag_alloc(offset,tid);
  alloc_time += __rdtsc() - pre;
  return res;
#else
#ifdef TAINT_COUNT
alloc_count++;
#endif
  return ssa_tag_alloc(offset,tid);
#endif
}

template <>
ssa_tag tag_combine(ssa_tag const &lhs, ssa_tag const &rhs,uint64_t tid)
{
#ifdef TAINT_PROFILE
  uint64_t pre =  __rdtsc();
  ssa_tag res = ssa_tag_combine(lhs,rhs,tid);
  combine_time += __rdtsc() - pre;
  return res;
#else
  return ssa_tag_combine(lhs,rhs,tid);
#endif
}