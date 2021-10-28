/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdbool.h>
#include <complex.h>
#include <stdlib.h>
#include <pthread.h>            // pthread_*
#include <liquid/liquid.h>      // cbuffercf_*
#include "config.h"
#ifndef HAVE_PTHREAD_BARRIERS
#include "pthread_barrier.h"
#endif
#include "block.h"
#include "util.h"               // XCALLOC, pthread_*_initialize, debug_print

#define BUF_SIZE_PROD_MTU_MULTIPLIER 8
#define BUF_SIZE_CONS_MRU_MULTIPLIER 2

static int32_t block_circ_buffer_init(struct circ_buffer *buffer, size_t buf_size) {
	ASSERT(buffer);
	buffer->buf = cbuffercf_create(buf_size);
	buffer->cond = XCALLOC(1, sizeof(pthread_cond_t));
	buffer->mutex= XCALLOC(1, sizeof(pthread_mutex_t));
	return pthread_cond_initialize(buffer->cond) || pthread_mutex_initialize(buffer->mutex);
}

static void block_circ_buffer_destroy(struct circ_buffer *buffer) {
	if(buffer != NULL) {
		cbuffercf_destroy(buffer->buf);
		XFREE(buffer->cond);
		XFREE(buffer->mutex);
		// No XFREE(buffer) as this is a member of a struct allocated by the caller
	}
}

static int32_t block_shared_buffer_init(struct shared_buffer *buffer, size_t buf_size, size_t thread_cnt) {
	ASSERT(buffer);
	ASSERT(thread_cnt > 0);
	buffer->buf = XCALLOC(buf_size, sizeof(float complex));
	buffer->data_ready = XCALLOC(1, sizeof(pthread_barrier_t));
	buffer->consumers_ready = XCALLOC(1, sizeof(pthread_barrier_t));
	return pthread_barrier_create(buffer->data_ready, thread_cnt) ||
		pthread_barrier_create(buffer->consumers_ready, thread_cnt);
}

static void block_shared_buffer_destroy(struct shared_buffer *buffer) {
	if(buffer != NULL) {
		XFREE(buffer->buf);
		XFREE(buffer->data_ready);
		XFREE(buffer->consumers_ready);
		// No XFREE(buffer) as this is a member of a struct allocated by the caller
	}
}

// Returns the number of successful connections made
int32_t block_connect_one2one(struct block *source, struct block *sink) {
	ASSERT(source);
	ASSERT(sink);
	ASSERT(source->producer.type == PRODUCER_SINGLE);
	ASSERT(sink->consumer.type == CONSUMER_SINGLE);
	ASSERT(source->producer.max_tu != 0);

	size_t buf_size_by_producer_mtu = BUF_SIZE_PROD_MTU_MULTIPLIER * source->producer.max_tu;
	size_t buf_size_by_consumer_mru = BUF_SIZE_CONS_MRU_MULTIPLIER * sink->consumer.min_ru;
	size_t buf_size = max(buf_size_by_producer_mtu, buf_size_by_consumer_mru);
	debug_print(D_MISC, "producer MTU: %zu consumer MRU: %zu buf_size: %zu\n",
			source->producer.max_tu, sink->consumer.min_ru, buf_size);
	NEW(struct block_connection, connection);
	int32_t ret = 0;
	if(block_circ_buffer_init(&connection->circ_buffer, buf_size) != 0) {
		goto end;
	}
	source->producer.out = sink->consumer.in = connection;
	ret = 1;
end:
	return ret;
}

void block_disconnect_one2one(struct block *source, struct block *sink) {
	ASSERT(source);
	ASSERT(sink);
	ASSERT(source->producer.type == PRODUCER_SINGLE);
	ASSERT(sink->consumer.type == CONSUMER_SINGLE);
	if(source->producer.out == sink->consumer.in) {
		block_circ_buffer_destroy(&source->producer.out->circ_buffer);
		XFREE(source->producer.out);
		source->producer.out = sink->consumer.in = NULL;
	}
}

int32_t block_connect_one2many(struct block *source, size_t sink_count, struct block *sinks[sink_count]) {
	ASSERT(source);
	ASSERT(sinks);
	ASSERT(source->producer.type == PRODUCER_MULTI);
	ASSERT(source->producer.max_tu != 0);

	size_t max_consumer_mru = 0;
	for(size_t i = 0; i < sink_count; i++) {
		ASSERT(sinks[i]->consumer.type == CONSUMER_MULTI);
		max_consumer_mru = max(max_consumer_mru, sinks[i]->consumer.min_ru);
	}
	size_t buf_size_by_producer_mtu = BUF_SIZE_PROD_MTU_MULTIPLIER * source->producer.max_tu;
	size_t buf_size_by_consumer_mru = BUF_SIZE_CONS_MRU_MULTIPLIER * max_consumer_mru;
	size_t buf_size = max(buf_size_by_producer_mtu, buf_size_by_consumer_mru);
	debug_print(D_MISC, "producer MTU: %zu max consumer MRU: %zu buf_size: %zu\n",
			source->producer.max_tu, max_consumer_mru, buf_size);

	NEW(struct block_connection, connection);
	int32_t ret = 0;
	// sink_count + 1 to account for the producer when initializing phread_barrier
	if(block_shared_buffer_init(&connection->shared_buffer, buf_size, sink_count + 1) != 0) {
		goto end;
	}
	source->producer.out = connection;
	for(size_t i = 0; i < sink_count; i++) {
		sinks[i]->consumer.in = connection;
		ret++;
	}
end:
	return ret;
}

void block_disconnect_one2many(struct block *source, size_t sink_count, struct block *sinks[sink_count]) {
	ASSERT(source);
	ASSERT(sinks);
	ASSERT(source->producer.type == PRODUCER_MULTI);

	struct block_connection *connection = source->producer.out;
	for(size_t i = 0; i < sink_count; i++) {
		if(sinks[i]->consumer.in == connection) {
			sinks[i]->consumer.in = NULL;
		}
	}
	block_shared_buffer_destroy(&connection->shared_buffer);
	XFREE(connection);
}

void block_connection_one2one_shutdown(struct block_connection *connection) {
	ASSERT(connection);
	pthread_mutex_lock(connection->circ_buffer.mutex);
	connection->flags |= BLOCK_CONNECTION_SHUTDOWN;
	pthread_mutex_unlock(connection->circ_buffer.mutex);
	pthread_cond_signal(connection->circ_buffer.cond);
}

void block_connection_one2many_shutdown(struct block_connection *connection) {
	ASSERT(connection);
	connection->flags |= BLOCK_CONNECTION_SHUTDOWN;
	pthread_barrier_wait(connection->shared_buffer.data_ready);
}

bool block_connection_is_shutdown_signaled(struct block_connection *connection) {
	ASSERT(connection);
	return connection->flags & BLOCK_CONNECTION_SHUTDOWN;
}

// Returns number of blocks successfully started
int32_t block_start(struct block *block) {
	ASSERT(block);
	ASSERT(block->thread_routine);
	int32_t ret = start_thread(&block->thread, block->thread_routine, block);
	if(ret == 0) {
		block->running = true;
		return 1;
	}
	return 0;
}

// Returns number of blocks successfully started
int32_t block_set_start(size_t block_cnt, struct block *block[block_cnt]) {
	ASSERT(block);
	int32_t ret = 0;
	for(size_t i = 0; i < block_cnt; i++) {
		ASSERT(block[i]->thread_routine);
		ret += block_start(block[i]);
	}
	return ret;
}

bool block_is_running(struct block *block) {
	ASSERT(block);
	return block->running;
}

bool block_set_is_any_running(size_t block_cnt, struct block *blocks[block_cnt]) {
	ASSERT(blocks);
	for(size_t i = 0; i < block_cnt; i++) {
		if(block_is_running(blocks[i])) {
			return true;
		}
	}
	return false;
}

