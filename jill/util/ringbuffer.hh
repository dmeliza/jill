/*
 * JILL - C++ framework for JACK
 *
 * includes code from klick, Copyright (C) 2007-2009  Dominic Sacre  <dominic.sacre@gmx.de>
 * additions Copyright (C) 2010 C Daniel Meliza <dmeliza@uchicago.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */
#ifndef _RINGBUFFER_HH
#define _RINGBUFFER_HH

#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <iostream>
#include <jack/ringbuffer.h>


namespace jill { namespace util {

/**
 *  Many JILL applications will access an audio stream in both the
 *  real-time thread and a lower-priority main thread.  This class,
 *  which is a thin wrapper around the JACK ringbuffer interface,
 *  allows simultaneous access by read and write threads.
 * 
 *  Client applications can derive from this class or encapsulate it
 *  to provide a wide variety of data handling functionality; note,
 *  however, that due to the performance demands of running in the
 *  real-time thread, none of the member functions are virtual.
 *
 *  @param T the type of object to store in the ringbuffer. Should be POD.
 */
template<typename T>
class Ringbuffer : boost::noncopyable
{
public:
	typedef T data_type;
	typedef std::size_t size_type;
	/** 
	 * Construct a ringbuffer with enough room to hold @a size
	 * objects of type T.
	 * 
	 * @param size The size of the ringbuffer (in objects)
	 */
	explicit Ringbuffer(size_type size) {
		_rb.reset(jack_ringbuffer_create(size * sizeof(T)),
			  jack_ringbuffer_free);
	}

	/** 
	 * Write data to the ringbuffer. 
	 * 
	 * @param src Pointer to source buffer
	 * @param nframes The number of frames in the source buffer
	 * 
	 * @return The number of frames actually written
	 */
	size_type push(const data_type *src, size_type nframes) {
		return jack_ringbuffer_write(_rb.get(), reinterpret_cast<char const *>(src), 
					     sizeof(data_type) * nframes) / sizeof(data_type);
	}

	/**
	 * Write data to the ringbuffer using a generator function.
	 *
	 * @param data_fun  function or object of type size_type(*)(*data_type, size_type)
	 * @param nframes The number of frames available, or 0 to use as many as
	 *                available and will fit (not impl)
	 * @return The number of frames actually written
	 */
	template <class Fun>
	size_type push(Fun &data_fun) {
		size_type nsamp = 0;
		jack_ringbuffer_data_t vec[2];
		jack_ringbuffer_get_write_vector(_rb.get(), vec);
		for (int i = 0; i < 2; ++i) {
			if (vec[i].len > 0)
				nsamp += data_fun(reinterpret_cast<data_type *>(vec[i].buf), vec[i].len);
			jack_ringbuffer_write_advance(_rb.get(), nsamp);
		}
		return nsamp;
	}
	/** 
	 * Read data from the ringbuffer. This version of the function
	 * copies data to a destination buffer.
	 * 
	 * @param dest The destination buffer. Needs to be pre-allocated
	 * @param nframes The number of frames to read
	 * 
	 * @return The number of frames actually read
	 */
	size_type pop(data_type *dest, size_type nframes=0) {
		if (nframes==0) 
			nframes = read_space();
		return jack_ringbuffer_read(_rb.get(), reinterpret_cast<char *>(dest), 
					    sizeof(data_type) * nframes) / sizeof(data_type);
	}

	/** 
	 * Read data from the ringbuffer. This version sets the input
	 * argument to the address of an array with the next block of
	 * data.  To free space after using the data, call
	 * advance(). Note that if the readable data spans the
	 * boundary of the ringbuffer, this call only provides access
	 * to the first contiguous chunk of data.
	 * 
	 * @param buf   Will point to data in ringbuffer after read
	 * 
	 * @return  The number of available samples in buf
	 */
	size_type peek(data_type **buf) {
		jack_ringbuffer_data_t vec[2];
		jack_ringbuffer_get_read_vector(_rb.get(), vec);
		for (int i = 0; i < 2; ++i) {
			if (vec[i].len > 0) {
				*buf = reinterpret_cast<data_type *>(vec[i].buf);
				return vec[i].len / sizeof(data_type);
			}
		}
		return 0;
	}

	/**
	 * Advance the read pointer by @a nframes, or up to the write
	 * pointer, whichever is less.
	 *
	 * @param nframes   The number of frames to advance. If 0, advance
	 *                  as far as possible
	 * @return the number of frames actually advanced
	 */
	size_type advance(size_type nframes=0) {
		// the underlying call can advance the read pointer past the write pointer
		nframes = (nframes==0) ? read_space() : std::min(read_space(), nframes);
		jack_ringbuffer_read_advance(_rb.get(), nframes * sizeof(data_type));
		return nframes;
	}

	/// @return the number of items that can be written to the ringbuffer
	size_type write_space() const {
		return jack_ringbuffer_write_space(_rb.get()) / sizeof(data_type);
	}

	/// @return the number of items that can be read from the ringbuffer
	size_type read_space() const {
		return jack_ringbuffer_read_space(_rb.get()) / sizeof(data_type);
	}

	size_type size() const { return read_space() + write_space(); }

private:
	boost::shared_ptr<jack_ringbuffer_t> _rb;
};

template<typename T>
std::ostream &operator<< (std::ostream &os, const Ringbuffer<T> &o)
{
	return os << "read space " << o.read_space() << "; write space " << o.write_space();
}

/**
 * The Prebuffer is a specialization of the Ringbuffer that
 * automatically flushes data when new data is added to maintain a
 * constant quantity of data to be read.  This is useful in
 * maintaining a prebuffer of some fixed time period.
 *
 * Note: this class is NOT thread-safe, because the push function
 * manipulates both the read and write pointers.
 */
template <typename T>
class Prebuffer : public Ringbuffer<T> {

public:
	typedef T data_type;
	typedef typename Ringbuffer<T>::size_type size_type;

	explicit Prebuffer(size_type size) : Ringbuffer<T>(size), _size(size) {}

	/**
	 * Push data onto the prebuffer. If the size of the data
	 * exceeds the size of the ringbuffer, only the last size
	 * items will be written.  The read pointer is advanced so
	 * that it is at most size behind the write pointer. Because
	 * of this, this operation is not reentrant with the read functions
	 *
	 * @param in  The input data
	 * @param nframes  The number of items in the data
	 * @return  The number of items actually written
	 */
	size_type push(const data_type *in, size_type nframes) {
		size_type nwrite = std::min(_size, nframes);
		int nflush = super::read_space() + nwrite - _size;
		if (nflush > 0)
			super::advance(nflush);
		return super::push(in+(nframes-nwrite), nwrite);
	}

private:
	typedef Ringbuffer<T> super;
	size_type _size;
};
	

}} // namespace jill::util


#endif // _DAS_RINGBUFFER_HH
