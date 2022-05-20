#pragma once
#include "pico/stdlib.h"

#ifndef RINGBUFFER_SIZE
#define RINGBUFFER_SIZE     4           // FIXME: should prob be a template parameter?
#endif

// not a full container, just a companion. manages the indices into an external buffer.
struct RingBuffer
{
    int     begin;      // read ptr, points to the first element
    int     end;        // write ptr, points to 1 past the last element
};


static inline void rb_init_ringbuffer(RingBuffer* rb)
{
    rb->begin = rb->end = 0;
}

static inline bool rb_is_empty(const RingBuffer* rb)
{
    return (rb->begin == rb->end);
}

static inline bool rb_is_full(const RingBuffer* rb)
{
    int end = (rb->end + 1) % RINGBUFFER_SIZE;
    return (rb->begin == end);
}

// returns index into external companion array that you can safely write your data into.
static inline int rb_push_back(RingBuffer* rb)
{
    // it's a mistake to try to add more into ringbuffer if it's full already.
    assert(!rb_is_full(rb));

    int r = rb->end;
    rb->end = (rb->end + 1) % RINGBUFFER_SIZE;
    return r;
}

static inline int rb_peek_front(RingBuffer* rb)
{
    // it's a mistake to call peek_front if ringbuffer is empty.
    assert(!rb_is_empty(rb));

    return rb->begin;
}

static inline int rb_peek_back(RingBuffer* rb)
{
    // it's a mistake to call peek_back if ringbuffer is empty.
    assert(!rb_is_empty(rb));

    return (rb->end + RINGBUFFER_SIZE - 1) % RINGBUFFER_SIZE;
}

static inline void rb_pop_front(RingBuffer* rb)
{
    assert(!rb_is_empty(rb));
    rb->begin = (rb->begin + 1) % RINGBUFFER_SIZE;
}


static inline void rb_unit_test()
{
    RingBuffer  rb;
    rb_init_ringbuffer(&rb);
    assert(rb_is_empty(&rb));
    assert(!rb_is_full(&rb));

    int i0 = rb_push_back(&rb);
    assert(!rb_is_empty(&rb));
    assert(!rb_is_full(&rb));

    assert(rb_peek_front(&rb) == i0);

    rb_pop_front(&rb);
    assert(rb_is_empty(&rb));

    // rb can store size-1 elements
    for (int i = 1; i < RINGBUFFER_SIZE; ++i)
    {
        int k = rb_push_back(&rb);
        assert(k == i);
        assert(!rb_is_empty(&rb));
        assert(rb_peek_back(&rb) == k);
    }
    assert(rb_is_full(&rb));

    for (int i = 1; i < RINGBUFFER_SIZE; ++i)
    {
        int k = rb_peek_front(&rb);
        assert(k == i);
        rb_pop_front(&rb);
        assert(!rb_is_full(&rb));
    }
    assert(rb_is_empty(&rb));
}
