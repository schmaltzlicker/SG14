 #pragma once
#include <utility>
#include <memory>
#include <algorithm>
#include "algorithm_ext.h"
//hot stands for hash, open-addressing w/ tombstone
//note: this class is untested and incomplete

struct default_open_addressing_load_algorithm
{
	size_t occupancy(size_t allocated)
	{
		auto half = allocated >> 1;
		return half + (half >> 2);
	}
	size_t allocated(size_t occupied)
	{
		return occupied + (occupied << 2);
	}
};

template<class It, class Pred>
It probe_forward(It begin, It start, It end, Pred f)
{
	auto It = start;
	for (unsigned pass = 0; pass < 2; ++pass)
	{
		do
		{
			if (f(It))
			{
				return It;
			}
			++It;
		} while (It != end);

		end = start;
		It = begin;
	}
	return end;
}

//alternating divergent series
template<class It, class Pred>
void probe_nearest(It begin, It current, It end, Pred op)
{
	auto It = start;
	ptrdiff_t offset = 1;
	while (It != end && It != begin - 1)
	{
		if (op(It))
		{
			return It;
		}
		It = start + offset;
		offset *= -1;
		offset += ptrdiff_t(offset > 0);
	};

	auto inc = It == end ? -1 : -1;
	It = start + offset;

	if (It == end)
	{
		end = begin - 1;
	}
	else
	{
		assert(It == begin);
	}

	while (It != end)
	{
		if (op(It))
		{
			return It;
		}
		It += inc;
	}
	return end;
}
namespace probe
{
	struct forward
	{
		template<class... T> auto operator()(T... s)  const
		{ return probe_forward(std::forward<T>(s)...); }
	};
	struct perfect
	{
		template<class It, class Pred> auto operator()(It begin, It start, It end, Pred p) const
		{
			p(s);
			return start;
		}
	};
	struct nearest
	{
		template<class... T> auto operator()(T... s) const
		{
			return probe_nearest(std::forward<T>(s)...);
		}
	};
}



template<class T, T key>
struct static_tombstone
{
	T operator()() const
	{
		return key;
	}
};

template<class K>
struct dynamic_tombstone
{
	K key;
	dynamic_tombstone() = default;
	dynamic_tombstone(const K& k)
		:key(k)
	{}
	dynamic_tombstone(K&& k)
		:key(std::move(k))
	{}

	K operator()() const
	{
		return key;
	}
};

template<
	class T,
	class Tomb,
	class Alloc = std::allocator<T>,
	class Hash = std::hash<T>,
	class Probe = probe::forward,
	class Cap = default_open_addressing_load_algorithm,
	class Eq = std::equal_to<T>
>
class hotset : public Probe, public Alloc, public Tomb
{
	T* mbegin;
	T* mend;
	size_t mcapacity;
	size_t moccupied;
	Hash hash;
	Cap cap;
	Eq eq;


	T* hash_at(const T& Item) const
	{
		return mbegin + (hash(Item) & ((mend-mbegin) - 1));
	}
	void init(size_t size)
	{
		if (size > 0)
		{
			//assert(powerOfTwo(size));
			allocate(size);
			std::fill(mbegin, mend, tombstone());
			moccupied = 0;
			mcapacity = cap.occupancy(mend - mbegin);
		}
	}

	template<class U>
	std::pair<T*, bool> insert_internal(U&& item)
	{
		auto Found = find_internal(item);
		*Found.first = std::forward<U>(item);
		return Found;
	}

	template<class U>
	std::pair<T*, bool> find_internal(const U& in) const
	{
		auto found = false;
		auto Invalid = tombstone();
		auto Eq = eq;
		auto op = [&found, &in, &Invalid, &Eq](T* at)
		{
			if (Eq(Invalid, *at))
			{
				found = false;
				return true;
			}
			else if (Eq(in, *at))
			{
				found = true;
				return true;
			}
			return false;
		};

		auto result_it = Probe::operator()(mbegin, hash_at(in), mend, op);
		return std::make_pair(result_it, found);
	}

	template<class U>
	T* rehash(U&& value)
	{
		auto oldbegin = mbegin;
		auto oldend = mend;
		auto size = std::max<size_t>(16, (mend-mbegin) << 1);
		mcapacity = cap.occupancy(size);

		mbegin = allocate(size, mbegin);
		mend = mbegin + size;
		std::fill(mbegin, mend, tombstone());
		auto Invalid = tombstone();
		auto it = oldbegin;
		while (it != oldend)
		{
			if (!eq(Invalid, *it))
			{
				auto h = hash_at(*it);
				insert_internal(std::move(*it));
			}
			++it;
		}
		deallocate(oldbegin, oldend-oldbegin);
		++moccupied;
		auto h = hash_at(value);
		return insert_internal(std::forward<U>(value)).first;
		
	}

	void partial_rehash(T* start)
	{
		const auto Invalid = tombstone();
		auto op = [this, &Invalid](T* at)
		{
			if (eq(*at, Invalid))
			{
				return true;
			}
			else
			{
				auto temp = std::move(*It);
				*It = Invalid;
				*find_internal(begin, start, end, c).first = std::move(temp);
				return false;
			}
		};

		Probe::operator()(begin, start, end, op);
	}
	void remove_internal(T* found)
	{
		--moccupied;
		*found = tombstone();
		partial_rehash(found + 1);
	}

public:
	hotset() = default;
	hotset(const hotset& in) = default;
	hotset(hotset&& in) = default;

	hotset(Tomb itomb, Probe p = Probe(), Hash h = Hash(), Cap c = Cap(), Alloc alloc = Alloc())
		: Probe(p)
		, hash(h)
		, cap(c)
		, Alloc(alloc)
		, Tomb(itomb)
		, moccupied(0)
		, mcapacity(0)
		, mbegin(nullptr)
		, mend(nullptr)
	{
	}

	bool is_invalid(const T& t) const
	{
		return eq(tombstone(), t);
	}
	size_t reserved() const
	{
		return mend - mbegin;
	}
	size_t capacity() const
	{
		return mcapacity;
	}
	size_t size() const
	{
		return moccupied;
	}

	template<class U>
	T* insert(U&& value)
	{
		if (mcapacity == moccupied)
		{
			return rehash(std::forward<U>(value));
		}
		auto result = insert_internal(std::forward<U>(value));
		moccupied += uint32_t(result.second == false);
		return result.first;
	}

	void erase(const T* value)
	{
		remove_internal(value);
	}
	bool erase(const T& value)
	{
		auto found = find_internal(item);
		if (found.second)
		{
			remove_internal(found.first);
			return true;
		}
		return false;
	}
	T tombstone() const
	{
		return Tomb::operator()();
	}
	bool empty() const
	{
		return moccupied == 0;
	}
	bool clear()
	{
		std::fill(begin, end, tombstone());
		moccupied = 0;
	}

	template<class U>
	const T* find(const U& value) const
	{
		auto result = find_internal(value);
		return result.second ? result.first : mend;
	}

	template<class U>
	bool contains(const U& value) const
	{
		return find_internal(value).second;
	}

	// this is poorly implemented due to restrictions on range-for loops.
	// efficiency can be improved notably when range-v3 enables heterogeneous iterators
	struct iterator : std::iterator< std::bidirectional_iterator_tag, T>
	{
		const hotset& set;
		T* pos;
		T* end;

		iterator(T* at, const hotset& inh)
			:pos(at-1), set(inh), end(inh.mend)
		{
			++(*this);
		}

		const T& operator*() const
		{
			return *pos;
		}
		iterator& operator++()
		{
			pos = std::find_if(pos + 1, end, [&](T& elem) { return !set.is_invalid(elem); });
			return *this;
		}
		bool operator!=(iterator other) const
		{
			return other.pos != pos;
		}
		bool operator==(iterator other) const
		{
			return other.pos == pos;
		}
		bool operator!=(T* other) const
		{
			return other != pos;
		}
		bool operator==(T* other) const
		{
			return other == pos;
		}
		const T* base() const
		{
			return pos;
		}
	};

	iterator begin() const
	{
		return iterator(mbegin, *this);
	}

	iterator end() const
	{
		return iterator(mend, *this);
	}
	~hotset()
	{
		stdext::destroy(mbegin, mend);
		Alloc::deallocate(mbegin, mend-mbegin);
	}


};

template<class T> using hodtset = hotset< T, dynamic_tombstone<T> >;
template<class T, T val> using hostset = hotset< T, static_tombstone<T, val> >;