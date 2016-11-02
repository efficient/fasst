/*`
 * A fixed-size pool that only supports two operations:
 * 1. get(): Allocate a new item
 * 2. clear(): Return all the items to the pool.
 */

#ifndef TRIVIAL_POOL_H
#define TRIVIAL_POOL_H

#include<assert.h>

#define TRIVIAL_POOL_DEBUG_ASSERT 0
#define trivial_pool_dassert(x) \
	do { if(TRIVIAL_POOL_DEBUG_ASSERT) assert(x); } while (0)

template <typename T> class trivial_pool {
public:

	/* Default constructor; call init(capacity) before use. */
	trivial_pool()
	{
		arr = NULL;
		index = -1;
		capacity = -1;
	}

	/* Actual constructor */
	void init(int _capacity)
	{
		/* Only initialized once */
		assert(arr == NULL && index == -1 && capacity == -1);
		assert(_capacity >= 1);

		arr = new T[_capacity];
		index = 0;
		capacity = _capacity;
	}

	~trivial_pool()
	{
		delete[] arr;
	}

	inline T* get()
	{
		trivial_pool_dassert(index < capacity);
		T* ret = &arr[index];
		index++;
		return ret;
	}

	inline int count()
	{
		return index;
	}

	inline void clear()
	{
		index = 0;
	}

private:
	T* arr;
	int index;
	int capacity;
};

#endif /* TRIVIAL_POOL_H */
