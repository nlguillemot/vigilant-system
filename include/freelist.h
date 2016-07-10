#pragma once

// freelist implementation based on http://bitsquid.blogspot.ca/2011/09/managing-decoupling-part-4-id-lookup.html

#include <cstdint>
#include <cassert>
#include <utility>

template<class T>
class freelist_t
{
    static const uint16_t index_mask = 0xFFFF;
    static const uint16_t tombstone = 0xFFFF;
    static const uint32_t new_object_id_add = 0x10000;

    struct index_t
    {
        uint32_t id;
        uint16_t index;
        uint16_t next;
    };

    size_t _num_objects;
    size_t _max_objects;
    size_t _cap_objects;
    T* _objects;
    uint32_t* _object_ids;
    index_t* _indices;
    uint16_t _enqueue;
    uint16_t _dequeue;

public:
	struct iterator
	{
		iterator(uint32_t* in)
		{
			_curr_object_id = in;
		}

		iterator& operator++()
		{
            _curr_object_id++;
			return *this;
		}

		uint32_t operator*()
		{
			return *_curr_object_id;
		}

		bool operator!=(const iterator& other) const
		{
			return _curr_object_id != other._curr_object_id;
		}

	private:
		uint32_t* _curr_object_id;
	};

    freelist_t()
    {
        _num_objects = 0;
        _max_objects = 0;
        _cap_objects = 0;
        _objects = nullptr;
        _object_ids = nullptr;
        _indices = nullptr;
        _enqueue = 0;
        _dequeue = -1;
    }

    freelist_t(size_t max_objects)
    {
		assert(max_objects < 0x10000);

        _num_objects = 0;
        _max_objects = max_objects;
        _cap_objects = max_objects;

        _objects = (T*)new char[max_objects * sizeof(T)];
        assert(_objects);

        _object_ids = (uint32_t*)new uint32_t[max_objects];
        assert(_object_ids);

        _indices = new index_t[max_objects];
        assert(_indices);

        for (size_t i = 0; i < max_objects; i++)
        {
            _indices[i].id = (uint32_t)i;
            _indices[i].next = (uint16_t)(i + 1);
        }
        
        if (max_objects > 0)
            _indices[max_objects - 1].next = 0;

        _enqueue = 0;
        _dequeue = (uint16_t)(max_objects - 1);
    }

    ~freelist_t()
    {
        for (size_t i = 0; i < _num_objects; i++)
        {
            _objects[i].~T();
        }
        delete[] _objects;
        delete[] _object_ids;
        delete[] _indices;
    }

    freelist_t(const freelist_t& other)
    {
        _num_objects = other._num_objects;
        _max_objects = other._max_objects;
        _cap_objects = other._cap_objects;
        
        _objects = (T*)new char[other._max_objects * sizeof(T)];
        assert(_objects);

        _object_ids = new uint32_t[other._max_objects];
        assert(_object_ids);

        _indices = new index_t[other._max_objects];
        assert(_indices);

        for (size_t i = 0; i < other._num_objects; i++)
        {
            new (_objects + i) T(*(other._objects + i));
            _object_ids[i] = other._object_ids[i];
        }

        for (size_t i = 0; i < other._max_objects; i++)
        {
            _indices[i] = other._indices[i];
        }

        _enqueue = other._enqueue;
        _dequeue = other._dequeue;
    }

    freelist_t& operator=(const freelist_t& other)
    {
        if (this != &other)
        {
            if (_cap_objects < other._max_objects)
            {
                this->~freelist_t();
                new (this) freelist_t(other);
            }
            else
            {
                for (size_t i = 0; i < other._num_objects; i++)
                {
                    if (i < _num_objects)
                    {
                        *(_objects + i) = *(other._objects + i);
                    }
                    else
                    {
                        new (_objects + i) T(*(other._objects + i));
                    }
                    _object_ids[i] = other._object_ids[i];
                }

                for (size_t i = 0; i < other._max_objects; i++)
                {
                    _indices[i] = other._indices[i];
                }

                _num_objects = other._num_objects;
                _max_objects = other._max_objects;
                _enqueue = other._enqueue;
                _dequeue = other._dequeue;
            }
        }
        return *this;
    }

    void swap(freelist_t& other)
    {
        using std::swap;
        swap(_num_objects, other._num_objects);
        swap(_max_objects, other._max_objects);
        swap(_cap_objects, other._cap_objects);
        swap(_objects, other._objects);
        swap(_object_ids, other._object_ids);
        swap(_indices, other._indices);
        swap(_enqueue, other._enqueue);
        swap(_dequeue, other._dequeue);
    }

    freelist_t(freelist_t&& other)
        : freelist_t()
    {
        swap(other);
    }

    freelist_t& operator=(freelist_t&& other)
    {
        if (this != &other)
        {
            swap(other);
        }
        return *this;
    }

    bool contains(uint32_t id) const
    {
        index_t& in = _indices[id & index_mask];
        return in.id == id && in.index != tombstone;
    }

    T& operator[](uint32_t id) const
    {
        return *(_objects + (_indices[id & index_mask].index));
    }

    uint32_t insert(const T& val)
    {
        index_t* in = insert_alloc();
        T* o = _objects + in->index;
        new (o) T(val);
        return in->id;
    }

    uint32_t insert(T&& val)
    {
        index_t* in = insert_alloc();
        T* o = _objects + in->index;
        new (o) T(std::move(val));
        return in->id;
    }

    template<class... Args>
    uint32_t emplace(Args&&... args)
    {
        index_t* in = insert_alloc();
        T* o = _objects + in->index;
        new (o) T(std::forward<Args>(args)...);
        return in->id;
    }

    void erase(uint32_t id)
    {
        assert(contains(id));

        index_t* in = &_indices[id & index_mask];

        T* o = _objects + in->index;
        _num_objects = _num_objects - 1;
        T* last = _objects + _num_objects;
        *o = std::move(*last);
        last->~T();
        _object_ids[in->index] = _object_ids[_num_objects];
        _indices[_object_ids[in->index] & index_mask].index = in->index;

        in->index = tombstone;
        _indices[_enqueue].next = id & index_mask;
        _enqueue = id & index_mask;
    }

	iterator begin() const
	{
		return iterator{ _object_ids };
	}

	iterator end() const
	{
		return iterator{ _object_ids + _num_objects };
	}

    bool empty() const
    {
        return _num_objects == 0;
    }

    size_t size() const
    {
        return _num_objects;
    }

    size_t capacity() const
    {
        return _max_objects;
    }

private:
    index_t* insert_alloc()
    {
        assert(_num_objects <= _max_objects);

        index_t* in = &_indices[_dequeue];
        _dequeue = in->next;
        in->id += new_object_id_add;
        in->index = (uint16_t)_num_objects++;
        _object_ids[in->index] = in->id;
        return in;
    }
};

template<class T>
typename freelist_t<T>::iterator begin(const freelist_t<T>& fl)
{
	return fl.begin();
}

template<class T>
typename freelist_t<T>::iterator end(const freelist_t<T>& fl)
{
	return fl.end();
}

template<class T>
void swap(freelist_t<T>& a, freelist_t<T>& b)
{
    a.swap(b);
}