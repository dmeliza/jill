/*
 * JILL - C++ framework for JACK
 *
 * Copyright (C) 2010 C Daniel Meliza <dmeliza@uchicago.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *! @file
 *! @brief Helper classes for the triggered_writer
 *! 
 */
#include "trigger_classes.hh"
#include <iostream>
using namespace jill;

/*
 * This is the file where we define the implementations of the classes
 * declared in in trigger_classes.hh (the interface).  The advantage
 * of separating the implementation from the interface is that the end
 * user doesn't need to see the implementation details.
 *
 * For example, we only ask the user to pass us a reference to a
 * soundfile writer. All the buffering is taken care of internally,
 * and we can change the details of how it's done without affecting
 * the interface.
 *
 *
 */

/*
 * This is the constructor for the TriggeredWriter class. We have to
 * assign the references in the initialization list (it's not
 * otherwise possible to reseat them).  We also initialize the
 * ringbuffer and store the size of the prebuffer.
 */
TriggeredWriter::TriggeredWriter(filters::WindowDiscriminator<sample_t> &wd, 
				 util::multisndfile &writer,
				 nframes_t prebuffer_size, nframes_t buffer_size)
	: _wd(wd), _writer(writer), _ringbuf(buffer_size), _prebuf(prebuffer_size) {}

/*
 * The function called by the process thread is quite simple. We don't
 * need to make real-time decisions about the state of the window, so
 * we just dump the data into the ringbuffer. This function is
 * essentially copied from writer.cc
 */
void 
TriggeredWriter::operator() (sample_t *in, sample_t *out, nframes_t nframes, nframes_t time)
{
	nframes_t nf = _ringbuf.push(in, nframes);
	// as in writer, we throw an error for buffer overruns. It may
	// be preferable to signal that an xrun has occurred and
	// simply invalidate the current file.
	if (nf < nframes)
		throw std::runtime_error("ringbuffer filled up");
	// should do something with timestamps
}

/*
 * The flush function is called by the main thread. It has several
 * jobs to do:
 *
 * 1. read samples from the ringbuffer
 * 2. push them to the window discriminator 
 * 3. depending on the state of the window discriminator, write data to disk
 * 4. advance the ringbuffer read pointer
 *
 * The prebuffering introduces some complications, because at the
 * moment the gate opens we need to access the samples before the
 * trigger point.  It would be nice to use the process ringbuffer, but
 * we run into issues at the boundary when the write pointer resets to
 * the beginning.  So we use a second buffer for the prebuffer data.
 *
 */
const std::string&
TriggeredWriter::flush()
{
	// read samples from buffer. We allocate a pointer and then
	// get the ringbuffer to point us at the memory where the data
	// are located.  
	sample_t *buf;
	nframes_t frames = _ringbuf.peek(&buf);

	// pass samples to window discriminator; its state may change,
	// in which case we will need to inspect the return value
	int offset = _wd.push(buf, frames);
	if (_wd.open()) {
		// gate is open; data before offset goes into
		// prebuffer. Some unnecessary copying in the interest
		// of simplicity.
		if (offset > 0) {
			_prebuf.push(buf, offset);
			_writer.next();
			//_prebuf.pop_fun(&util::multisndfile::push)
			_writer.write(buf+offset, frames-offset);
		}
		else
			_writer.write(buf, frames);
	}
	else {	
		// gate is closed; Data before offset goes to file;
		// rest goes to prebuffer
		if (offset > 0) {
			_writer.write(buf, offset);
			_prebuf.push(buf+offset, frames-offset);
		}
		else
			_prebuf.push(buf, frames);
	}
	_ringbuf.advance(frames);
	return _writer.current_file();
}


TriggerOptions::TriggerOptions(const char *program_name, const char *program_version)
	: Options(program_name, program_version) // this calls the superclass constructor
{
	cmd_opts.add_options()
		("output_file", po::value<std::string>(), "set output file name template");
	pos_opts.add("output_file", -1);
} 

void 
TriggerOptions::process_options() 
{
	if (vmap.count("output_file"))
		output_file_tmpl = get<std::string>("output_file");
	else {
		std::cerr << "Error: missing required output file name " << std::endl;
		throw Exit(EXIT_FAILURE);
	}
}

void 
TriggerOptions::print_usage() 
{
	std::cout << "Usage: " << _program_name << " [options] output_file\n\n"
		  << "output_file can be any file format supported by libsndfile\n"
		  << visible_opts;
}


