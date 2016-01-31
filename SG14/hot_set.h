 #pragma once
#include <utility>
#include <memory>
#include <algorithm>
#include "algorithm_ext.h"
//hot stands for hash, open-addressing w/ tombstone
//note: this class is untested and incomplete

struct default_open_addressing_load_algorithm
{
	//how many elements can fit in this many buckets
	size_t occupancy(size_t allocated)
	{
		auto half = allocated >> 1;
		return half + (half >> 2);
	}

	//how many buckets needed to fulfill this many elements
	size_t allocated(size_t occupied)
	{
		occupied += occupied;
		auto l = log2(occupied);
		auto c = ceil(l);
		return std::max(16, 1 << int(c));
	}
};

template<class It, class Pred>
It probe_nearest(It begin, It start, It end, Pred op)
{
	if (op(*start))
	{
		return start;
	}
	auto ItBack = start + 1;
	--start;
	auto fend = begin - 1;
	while (start != fend || ItBack != end)
	{
		if (start != fend)
		{
			if (op(*start))
			{
				return start;
			}
			--start;
		}
		if (ItBack != end)
		{
			if (op(*ItBack))
			{
				return ItBack;
			}
			++ItBack;
		}
	};

	return end;
}

//probing algorithms
//class wrappers provide easy template deduction
namespace probe
{
	struct perfect
	{
		template<class It, class Pred> auto operator()(It begin, It start, It end, Pred p) const
		{
			p(*start);
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


// generator functions
template<class T, T key>
struct static_value
{
	T operator()() const
	{
		return key;
	}
};

template<class K>
struct dynamic_value
{
	K key;
	dynamic_value()
		:key()
	{}
	dynamic_value(const K& k)
		:key(k)
	{}
	dynamic_value(K&& k)
		:key(std::move(k))
	{}

	K operator()() const
	{
		return key;
	}
};

template<
	class T,					//contained type
	class Tomb,					//tombstone generator function
	class Eq = std::equal_to<void>,//element comparator
	class Alloc = std::allocator<T>,
	class Hash = std::hash<T>,	
	//load algorithm, must give allocations with power-of-two # of elements
	class Cap = default_open_addressing_load_algorithm, 
	class Probe = probe::nearest	//determines how elements are probed for collisions
>
class hot_set : public Probe, public Alloc, public Tomb
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
			mbegin = allocate(size);
			mend = mbegin + size;
			std::uninitialized_fill(mbegin, mend, tombstone());
			moccupied = 0;
			mcapacity = cap.occupancy(size);
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
		auto Eq = eq;
		auto op = [&found, &in, this, Eq](const T& at)
		{
			if (Eq(tombstone(), at))
			{
				found = false;
				return true;
			}
			else if (Eq(in, at))
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
		auto size = std::max<size_t>(32, (oldend-oldbegin) << 1);
		mcapacity = cap.occupancy(size);

		mbegin = allocate(size);
		mend = mbegin + size;
		std::fill(mbegin, mend, tombstone());
		auto Invalid = tombstone();
		auto it = oldbegin;
		auto Eq = eq;
		while (it != oldend)
		{
			if (!Eq(Invalid, *it))
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
		auto op = [this, tomb{ tombstone() }, Eq{ eq }](T& at)
		{
			if (Eq(at, tomb))
			{
				return true;
			}
			else
			{
				auto temp = std::move(at);
				at = tomb;
				auto put = find_internal(temp).first;
				*put = std::move(temp);
				return false;
			}
		};
		if (start == mend)
			start = mbegin;
		Probe::operator()(mbegin, start, mend, op);
	}
	void remove_internal(T* found)
	{
		--moccupied;
		*found = tombstone();
		partial_rehash(found + 1);
	}

public:
	// this is poorly implemented due to restrictions on range-for loops.
	// efficiency can be improved notably when range-v3 enables heterogeneous iterators
	struct iterator : std::iterator< std::bidirectional_iterator_tag, T>
	{
		const hot_set& set;
		T* pos;
		T* end;

		iterator(T* at, const hot_set& inh)
			:pos(at), set(inh), end(inh.mend)
		{
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

	hot_set()
		: mbegin()
		, mend()
		, mcapacity()
		, moccupied()
	{}

	hot_set(const hot_set& in)
		: Probe(in)
		, Alloc(in)
		, Tomb(in)
		, mcapacity(in.mcapacity)
		, moccupied(in.moccupied)
		, hash(in.hash)
		, cap(in.cap)
		, eq(in.eq)
	{
		auto size = in.reserved();
		mbegin = allocate(size);
		mend = mbegin + size;
		std::copy(in.mbegin, in.mend, mbegin);
	}

	hot_set(hot_set&& in)
		:mbegin(in.mbegin)
		, mend(in.mend)
		, mcapacity(in.mcapacity)
		, moccupied(in.moccupied)
		, Probe(std::move(in))
		, Alloc(std::move(in))
		, hash(std::move(in.hash))
		, cap(std::move(in.cap))
		, eq(std::move(in.eq))
	{
		in.mcapacity = 0;
		in.moccupied = 0;
		in.mbegin = nullptr;
		in.mend = nullptr;
	}

	hot_set(size_t init_capacity, Tomb itomb = Tomb(),  Probe p = Probe(), Hash h = Hash(), Cap c = Cap(), Alloc alloc = Alloc())
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
		init(cap.allocated(init_capacity));
	}

	bool is_invalid(const T& t) const
	{
		return eq(tombstone(), t);
	}

	//number of elements allocated by the set
	size_t reserved() const
	{
		return mend - mbegin;
	}

	//number of elements the set may contain before reallocating
	size_t capacity() const
	{
		return mcapacity;
	}

	//number of elements in the set
	size_t size() const
	{
		return moccupied;
	}

	//Inserts an element into the set
	//If size() == capacity(), invalidates any iterators
	template<class U>
	iterator insert(U&& value)
	{
		if (mcapacity == moccupied)
		{
			return iterator(rehash(std::forward<U>(value)), *this);
		}
		auto result = insert_internal(std::forward<U>(value));
		moccupied += uint32_t(result.second == false);
		return iterator(result.first, *this);
	}

	//Removes element. invalidates all iterators.
	void erase(iterator value)
	{
		remove_internal(value.base());
	}

	//removes element. invalidates all iterators.
	bool erase(const T& value)
	{
		auto found = find_internal(value);
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

	//invalidates all iterators
	bool clear()
	{
		std::fill(begin, end, tombstone());
		moccupied = 0;
	}

	template<class U>
	iterator find(const U& value) const
	{
		auto result = find_internal(value);
		return iterator(result.second ? result.first : mend, *this);
	}

	template<class U>
	bool contains(const U& value) const
	{
		return find_internal(value).second;
	}

	iterator begin() const
	{
		auto result = iterator(mbegin-1, *this);
		++result;
		return result;
	}

	iterator end() const
	{
		return iterator(mend, *this);
	}

	//could be useful for iterating elements more efficiently. User must manually skip tombstones.
	//array_view<T> unsafe_view(){ return {mbegin, mend}; }

	~hot_set()
	{
		stdext::destroy(mbegin, mend);
		Alloc::deallocate(mbegin, mend-mbegin);
	}


};

template<class K, class V>
struct hot_pair
{
	K key;
	V value;
	constexpr hot_pair()
		:key(), value()
	{}

	constexpr hot_pair(const K& k)
		:key(k)
		,value()
	{}
	template<class KK, class VV>
	constexpr hot_pair(KK&& k, VV&& v)
		: key(std::forward<KK>(k)), value(std::forward<VV>(v))
	{}

	operator const K&() const
	{
		return key;
	}
	bool operator==(const hot_pair<K, V>& other) const
	{
		return other.key == key && other.value == value;
	}
};
template<class K, class V>
struct hot_pair_by_key
{
	bool operator()(const hot_pair<K,V>& a, const hot_pair<K,V>& b) const
	{
		return a.key == b.key;
	}
};


namespace std
{
	template<class K, class V>
	struct hash<hot_pair<K, V>>
	{
		size_t operator()(const hot_pair<K, V>& p) const
		{
			std::hash<K> h;
			return h(p.key);
		}
	};
}



template<class T> using hod_set = hot_set< T, dynamic_value<T> >;
template<class T, T tombstone> using hos_set = hot_set< T, static_value<T, tombstone> >;

//Map implementation, unique keys
//The user provides a key which shall never be inserted

template<
	class K, class V,
	class Tomb,	
	class Eq = std::equal_to<void>,
	class Alloc = std::allocator<hot_pair<K,V>>,
	class Hash = std::hash<K>,
	class Cap = default_open_addressing_load_algorithm,
	class Probe = probe::nearest
>
class hot_map : public hot_set<hot_pair<K,V>, Tomb, Eq, Alloc, Hash, Cap, Probe>
{
	typedef hot_set<hot_pair<K, V>, Tomb, Eq, Alloc, Hash, Cap, Probe> Super;
public:
	hot_map() = default;
	hot_map(hot_map&&) = default;
	hot_map(const hot_map&) = default;
	hot_map(size_t init_capacity, K tombstone_key, V tombstone_value,  Probe p = Probe(), Hash h = Hash(), Cap c = Cap(), Alloc alloc = Alloc())
		: Super(init_capacity, hot_pair<K,V>(std::move(tombstone_key), std::move(tombstone_value)),  p, h, c, alloc)
	{}

	template<class KK, class VV>
	iterator insert(KK&& key, VV&& value)
	{
		return Super::insert(hot_pair<K, V>{std::forward<KK>(key), std::forward<VV>(value)});
	}
};

template<class K, class V> using hod_map = hot_map<K, V, dynamic_value<K> , hot_pair_by_key<K,V>>;
template<class K, K tombstone, class V> using hos_map = hot_map< K, V, static_value<K, tombstone>, hot_pair_by_key<K,V>>;


//Map implementation, duplicate keys allowed
//the user provides a key-value pair which shall never be inserted
template<
	class K, class V,
	class Tomb,
	class Eq = std::equal_to<void>,
	class Alloc = std::allocator<hot_pair<K, V>>,
	class Hash = std::hash<K>,
	class Cap = default_open_addressing_load_algorithm,
	class Probe = probe::nearest
>
class hot_multimap : public hot_map<K, V, Tomb, Eq, Alloc, Hash, Cap, Probe>
{
	typedef hot_map<K, V, Tomb, Eq, Alloc, Hash, Cap, Probe> Super;
public:
	hot_multimap() = default;
	hot_multimap(hot_multimap&&) = default;
	hot_multimap(const hot_multimap&) = default;

	hot_multimap(size_t init_capacity, K tombstone_key, V tombstone_value, Probe p = Probe(), Hash h = Hash(), Cap c = Cap(), Alloc alloc = Alloc())
		: Super(init_capacity, std::move(tombstone_key), std::move(tombstone_value), p, h, c, alloc)
	{}

	template<class KK, class VV>
	iterator find(KK&& key, VV&& val) const
	{
		return Super::find(hot_pair<K, V>{std::forward<KK>(key), std::forward<VV>(val)});
	}
};


template<class K, class V> using hod_multimap = hot_multimap<K, V, dynamic_value<hot_pair<K, V>> >;