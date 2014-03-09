
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <pthread.h>
#include <sys/time.h>

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>

#define FPS_THRESHOLD 2000.0

struct plane_t {
	uint32_t offset;
	uint32_t pitch;
	uint32_t height;
};

struct data_t {
	struct MMAL_COMPONENT_T *component;
	struct MMAL_PORT_T *input;
	struct MMAL_POOL_T *pool;
	int buffer_num;
	int width;
	int height;
	struct plane_t layout[3];
	uint32_t image_size;
	int line_x;
	int line_y;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int buffers_in_use;
};

static volatile sig_atomic_t aborted = 0;

static void on_signal(int sig);
double millisecs(void);
static uint32_t align(uint32_t x, uint32_t y);
static void render_image(const struct data_t *data, uint8_t *image);
static void control_port_cb(struct MMAL_PORT_T *port, struct MMAL_BUFFER_HEADER_T *buffer);
static void input_port_cb(struct MMAL_PORT_T *port, struct MMAL_BUFFER_HEADER_T *buffer);
static void* pool_allocator_alloc(void *context, uint32_t size);
static void pool_allocator_free(void *context, void *mem);

int main(int argc, char *argv[]) {
	struct data_t data;
	MMAL_STATUS_T status;
	MMAL_BUFFER_HEADER_T *buf;
	int frames = 0;
	double t1, t2;
	int i;
	int ret = EXIT_SUCCESS;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	memset(&data, 0, sizeof(struct data_t));

	if(argc > 1) {
		if((strcmp(argv[1], "-h") == 0) || (strcmp(argv[1], "--help") == 0)) {
			printf("usage: %s [buffer_num] [width] [height]\n", argv[0]);
			goto out;
		}

		data.buffer_num = atoi(argv[1]);
	}

	if(data.buffer_num <= 2) {
		data.buffer_num = 2;
	}

	if(argc > 2) {
		data.width = atoi(argv[2]);
	}

	if(data.width <= 0) {
		data.width = 1920;
	}

	if(argc > 3) {
		data.height = atoi(argv[3]);
	}

	if(data.height <= 0) {
		data.height = 1080;
	}

	pthread_mutex_init(&data.mutex, NULL);
	pthread_cond_init(&data.cond, NULL);

	status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &data.component);
	if(status != MMAL_SUCCESS) {
		printf("Failed to create component %s (%x, %s)\n", MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, status, mmal_status_to_string(status));
		ret = EXIT_FAILURE;
		goto out;
	}

	data.component->control->userdata = (struct MMAL_PORT_USERDATA_T *)&data;
	status = mmal_port_enable(data.component->control, control_port_cb);
	if(status != MMAL_SUCCESS) {
		printf("Failed to enable control port %s (%x, %s)\n", data.component->control->name, status, mmal_status_to_string(status));
		ret = EXIT_FAILURE;
		goto out;
	}

	data.input = data.component->input[0];
	data.input->userdata = (struct MMAL_PORT_USERDATA_T *)&data;
	data.input->buffer_num = data.buffer_num;
	data.input->format->encoding = MMAL_ENCODING_I420;
	data.input->format->es->video.width = data.width;
	data.input->format->es->video.height = data.height;
	data.input->format->es->video.crop.x = 0;
	data.input->format->es->video.crop.y = 0;
	data.input->format->es->video.crop.width = data.width;
	data.input->format->es->video.crop.height = data.height;

	data.layout[0].offset = 0;
	for(i = 0; i < 3; ++i) {
		data.layout[i].pitch = align(data.width, 32);
		data.layout[i].height = align(data.height, 16);
		if(i > 0) {
			data.layout[i].offset = data.layout[i - 1].offset + data.layout[i - 1].pitch * data.layout[i - 1].height;
			data.layout[i].pitch /= 2;
			data.layout[i].height /= 2;
		}
	}
	data.image_size = data.layout[2].offset + data.layout[2].pitch * data.layout[2].height;

	status = mmal_port_format_commit(data.input);
	if(status != MMAL_SUCCESS) {
		printf("Failed to commit input port format (%x, %s)\n", status, mmal_status_to_string(status));
		ret = EXIT_FAILURE;
		goto out;
	}

	status = mmal_port_enable(data.input, input_port_cb);
	if(status != MMAL_SUCCESS) {
		printf("Failed to enable input port %s (%d, %s)\n", data.input->name, status, mmal_status_to_string(status));
		ret = EXIT_FAILURE;
		goto out;
	}

	status = mmal_component_enable(data.component);
	if(status != MMAL_SUCCESS) {
		printf("Failed to enable component %s (%d, %s)\n", data.component->name, status, mmal_status_to_string(status));
		ret = EXIT_FAILURE;
		goto out;
	}

	data.pool = mmal_pool_create_with_allocator(data.buffer_num, data.image_size, &data, pool_allocator_alloc, pool_allocator_free);
	if(!data.pool) {
		printf("Failed to create pool (%d, %s)\n", status, mmal_status_to_string(status));
		ret = EXIT_FAILURE;
		goto out;
	}

	t1 = millisecs();
	while(!aborted) {
		buf = mmal_queue_wait(data.pool->queue);

		buf->type->video.planes = 3;
		buf->type->video.flags = 0;
		for(i = 0; i < 3; ++i) {
			buf->type->video.offset[i] = data.layout[i].offset;
			buf->type->video.pitch[i] = data.layout[i].pitch;
		}
		buf->type->video.offset[3] = 0;
		buf->type->video.pitch[3] = 0;

		render_image(&data, buf->data);
		buf->length = data.image_size;
		buf->flags = MMAL_BUFFER_HEADER_FLAG_FRAME;

		pthread_mutex_lock(&data.mutex);
		while(data.buffers_in_use > 1) {
			pthread_cond_wait(&data.cond, &data.mutex);
		}
		mmal_port_send_buffer(data.input, buf);
		++data.buffers_in_use;
		pthread_mutex_unlock(&data.mutex);

		data.line_x = (data.line_x + 2) % data.width;
		data.line_y = (data.line_y + 2) % data.height;
		++frames;

		t2 = millisecs();
		if(t2 - t1 >= FPS_THRESHOLD) {
			printf("fps: %lf\n", 1000 * frames / (t2 - t1));
			frames = 0;
			t1 = t2;
		}
	}

out:
	if(data.component) {
		if(data.component->is_enabled) {
			mmal_component_disable(data.component);
		}

		if(data.component->control->is_enabled) {
			mmal_port_disable(data.component->control);
		}
	}

	if(data.input && data.input->is_enabled) {
		mmal_port_disable(data.input);
	}

	if(data.pool) {
		while(mmal_queue_length(data.pool->queue) != data.buffer_num) {
			printf("Waiting for %d buffers ...\n", data.buffer_num - mmal_queue_length(data.pool->queue));
			usleep(1000000);
		}

		mmal_pool_destroy(data.pool);
	}

	if(data.component) {
		mmal_component_release(data.component);
	}

	pthread_cond_destroy(&data.cond);
	pthread_mutex_destroy(&data.mutex);

	return ret;
}

static void on_signal(int sig)
{
	if(aborted) {
		abort();
	}

	aborted = 1;
}

double millisecs(void) {
	struct timeval tv;
	double result = 0;

	if(gettimeofday(&tv, NULL) == 0) {
		result = (tv.tv_sec * 1000) + (tv.tv_usec / 1000.0);
	}

	return result;
}

static uint32_t align(uint32_t x, uint32_t y) {
	uint32_t mod = x % y;

	if(mod == 0) {
		return x;
	}
	else {
		return x + y - mod;
	}
}

static void render_image(const struct data_t *data, uint8_t *image) {
	const struct plane_t *layout = data->layout;
	uint8_t *y = image + layout[0].offset;
	uint8_t *u = image + layout[1].offset;
	uint8_t *v = image + layout[2].offset;
	int width = data->width;
	int height = data->height;
	int line_x = data->line_x;
	int line_y = data->line_y;
	uint8_t col;
	int i;

	/* Green (Y=149, U=43,  V=21 ): non-visible parts of the buffer
	 * White (Y=255, U=128, V=128): background
	 * Red   (Y=76,  U=84,  V=255): moving line
	 */

	for(i = 0; i < layout[0].height; ++i, y += layout[0].pitch) {
		if((i / 2) == (line_y / 2)) {
			col = 76;
		}
		else if(i < height) {
			col = 255;
		}
		else {
			col = 149;
		}

		memset(y, col, line_x);
		memset(y + line_x, 76, 2);
		memset(y + line_x + 2, col, width - line_x - 2);
		memset(y + width, 149, layout[0].pitch - width);
	}

	for(i = 0; i < layout[1].height; ++i, u += layout[1].pitch) {
		if(i == (line_y / 2)) {
			col = 84;
		}
		else if(i < height / 2) {
			col = 128;
		}
		else {
			col = 43;
		}

		memset(u, col, line_x / 2);
		memset(u + line_x / 2, 84, 1);
		memset(u + line_x / 2 + 1, col, width / 2 - line_x / 2 - 1);
		memset(u + width / 2, 43, layout[1].pitch - width / 2);
	}

	for(i = 0; i < layout[2].height; ++i, v += layout[2].pitch) {
		if(i == (line_y / 2)) {
			col = 255;
		}
		else if(i < height / 2) {
			col = 128;
		}
		else {
			col = 21;
		}

		memset(v, col, line_x / 2);
		memset(v + line_x / 2, 255, 1);
		memset(v + line_x / 2 + 1, col, width / 2 - line_x / 2 - 1);
		memset(v + width / 2, 21, layout[2].pitch - width / 2);
	}
}

static void control_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	MMAL_STATUS_T status;

	if(buffer->cmd == MMAL_EVENT_ERROR) {
		status = *(MMAL_STATUS_T *)buffer->data;
		printf("control_port_cb(%p, %p): MMAL_EVENT_ERROR status=%x \"%s\"\n", port, buffer, (int)status, mmal_status_to_string(status));
	}

	mmal_buffer_header_release(buffer);
}

static void input_port_cb(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
	struct data_t * data = (struct data_t *)port->userdata;
	pthread_mutex_lock(&data->mutex);
	mmal_buffer_header_release(buffer);
	--data->buffers_in_use;
	if(data->buffers_in_use <= 1) {
		pthread_cond_signal(&data->cond);
	}
	pthread_mutex_unlock(&data->mutex);
}

static void* pool_allocator_alloc(void *context, uint32_t size)
{
	struct data_t *data = (struct data_t *)context;
	return mmal_port_payload_alloc(data->input, size);
}

static void pool_allocator_free(void *context, void *mem)
{
	struct data_t *data = (struct data_t *)context;
	mmal_port_payload_free(data->input, (uint8_t *)mem);
}
