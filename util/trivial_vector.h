/*`
 * A fixed-size vector that only supports two operations:
 * 1. push_back: Add an item to the back
 * 2. clear(): Clear all items
 */

#ifndef TRIVIAL_VECTOR_H
#define TRIVIAL_VECTOR_H

#include<assert.h>
#include<stdio.h>

#define TRIVIAL_VECTOR_DEBUG_ASSERT 0
#define trivial_vector_dassert(x) \
	do { if(TRIVIAL_VECTOR_DEBUG_ASSERT) assert(x); } while (0)

template <typename T> class trivial_vector {
public:

	/* Default constructor; call init(capacity) before use. */
	trivial_vector()
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
		index = 0;	/* @index is the current empty slot */
		capacity = _capacity;
	}

	~trivial_vector()
	{
		printf("Destroying trivial vector\n");
		delete[] arr;
	}

	inline void push_back(T t)
	{
		trivial_vector_dassert(index < capacity);
		arr[index] = t;
		index++;
	}

	inline T operator [] (int i)
	{
		trivial_vector_dassert(i < index);
		return arr[i];
	}

	inline int size()
	{
		return index;
	}

	inline void clear()
	{
		index = 0;
	}

	T* arr;
	int index;
	int capacity;
};

#endif /* TRIVIAL_VECTOR_H */
