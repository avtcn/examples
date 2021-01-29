// #include "v4l2-avt-ioctl.h" //for older version of the Jetpack 4.4 Beta version
#include "libcsi_ioctl.h"
#include <signal.h>
#include <assert.h>
#include <byteswap.h>
#include <errno.h>
#include <fcntl.h> /* low-level i/o */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <linux/videodev2.h>


#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
  void *start;
  size_t length;
};

static void errno_exit(const char *s) {
  fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
  exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg) {
  int r;

  do {
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);

  return r;
}

static void save_frame(const char *path, const void *p, int size) {
  int fd, rz;
  printf("save frame\n");
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (fd < 0)
    perror("open");
  else {
    rz = write(fd, p, size);
    printf("Wrote %d of %d bytes to %s\n", rz, size, path);
    close(fd);
  }
}

bool running;
void handle_sigint(int) {
  running = false;
}

int main(int argc, char **argv) {
  static char *dev_name;
  int width, height;
  static int fd = -1;
  struct stat st;
  struct buffer *buffers;
  static unsigned int n_buffers;
  enum v4l2_buf_type type;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_streamparm parm;
  struct v4l2_input input;
  v4l2_std_id std_id;
  struct v4l2_buffer buf;
  unsigned int i;
  char filename[32];

  struct timeval start_time, end_time;
  long milli_time, seconds, useconds;
  int counter=0;


  /* parse args */
  if (argc < 2) {
    //fprintf(stderr, "usage: %s <device> <width> <height> [options]\n", argv[0]);
    fprintf(stderr, "usage: %s <device> [options]\n", argv[0]);
    fprintf(stderr, "  options:\n");
    fprintf(stderr, "    --software: set trigger source to software (default)\n");
    fprintf(stderr, "    --line0:    set trigger source to hardware line 0\n");
    fprintf(stderr, "    --line1:    set trigger source to hardware line 1\n");
    fprintf(stderr, "    --rising:   set trigger activation to rising edge (default)\n");
    fprintf(stderr, "    --falling:  set trigger activation to falling edge\n");
    fprintf(stderr, "    --any:      set trigger activation to any edge\n");
    fprintf(stderr, "    --high:     set trigger activation to high level\n");
    fprintf(stderr, "    --low:      set trigger activation to low level\n");
    exit(1);
  }

  dev_name = argv[1];
//  width = atoi(argv[2]);
//  height = atoi(argv[3]);

  int source = V4L2_TRIGGER_SOURCE_SOFTWARE;
  int activation = V4L2_TRIGGER_ACTIVATION_RISING_EDGE;

  for(int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--software") == 0) {
      source = V4L2_TRIGGER_SOURCE_SOFTWARE;
    } else if (strcmp(argv[i], "--line0") == 0) {
      source = V4L2_TRIGGER_SOURCE_LINE0;
    } else if (strcmp(argv[i], "--line1") == 0) {
      source = V4L2_TRIGGER_SOURCE_LINE1;
    } else if (strcmp(argv[i], "--rising") == 0) {
      activation = V4L2_TRIGGER_ACTIVATION_RISING_EDGE;
    } else if (strcmp(argv[i], "--falling") == 0) {
      activation = V4L2_TRIGGER_ACTIVATION_FALLING_EDGE;
    } else if (strcmp(argv[i], "--any") == 0) {
      activation = V4L2_TRIGGER_ACTIVATION_ANY_EDGE;
    } else if (strcmp(argv[i], "--high") == 0) {
      activation = V4L2_TRIGGER_ACTIVATION_LEVEL_HIGH;
    } else if (strcmp(argv[i], "--low") == 0) {
      activation = V4L2_TRIGGER_ACTIVATION_LEVEL_LOW;
    }
  }


  /* open device */
  fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
  if (-1 == fd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_name, errno,
            strerror(errno));
    exit(EXIT_FAILURE);
  }

  /* ensure device has video capture capability */
  if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s is no V4L2 device\n", dev_name);
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_QUERYCAP");
    }
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is no video capture device\n", dev_name);
    exit(EXIT_FAILURE);
  }
  if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
    fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
    exit(EXIT_FAILURE);
  }

  /* set framerate */
  CLEAR(parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  parm.parm.capture.timeperframe.denominator = 5000;
  parm.parm.capture.timeperframe.numerator = 1000;

  if (-1 == xioctl(fd, VIDIOC_S_PARM, &parm))
    perror("VIDIOC_S_PARM");

  /* get framerate */
  CLEAR(parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_G_PARM, &parm))
    perror("VIDIOC_G_PARM");

  /* set format */
  CLEAR(fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
    errno_exit("VIDIOC_G_FMT");
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  width = fmt.fmt.pix.width;
  height = fmt.fmt.pix.height;
  //fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_XRGB32;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
    errno_exit("VIDIOC_S_FMT");

  /* get and display format */
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
    errno_exit("VIDIOC_G_FMT");
  printf("%s: %dx%d %c%c%c%c %2.2ffps\n", dev_name, fmt.fmt.pix.width,
         fmt.fmt.pix.height, (fmt.fmt.pix.pixelformat >> 0) & 0xff,
         (fmt.fmt.pix.pixelformat >> 8) & 0xff,
         (fmt.fmt.pix.pixelformat >> 16) & 0xff,
         (fmt.fmt.pix.pixelformat >> 24) & 0xff,
         (float)parm.parm.capture.timeperframe.denominator /
             (float)parm.parm.capture.timeperframe.numerator);

  /* request buffers */
  printf("request buffers\n");
  CLEAR(req);
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s does not support " "memory mapping\n", dev_name);
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_REQBUFS");
    }
  }
  printf("%d buffers obtained\n", req.count);

  /* allocate buffers */
  printf("allocate buffers\n");
  buffers = (struct buffer *)calloc(req.count, sizeof(*buffers));
  if (!buffers) {
    fprintf(stderr, "Out of memory\n");
    exit(EXIT_FAILURE);
  }

  /* mmap buffers */
  for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
    struct v4l2_buffer buf;

    CLEAR(buf);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = n_buffers;

    if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
      errno_exit("VIDIOC_QUERYBUF");

    buffers[n_buffers].length = buf.length;
    buffers[n_buffers].start =
        mmap(NULL /* start anywhere */, buf.length,
             PROT_READ | PROT_WRITE /* required */,
             MAP_SHARED /* recommended */, fd, buf.m.offset);

    if (MAP_FAILED == buffers[n_buffers].start)
      errno_exit("mmap");

    printf("buffers[n_buffers].start= %p\t buffers[n_buffers].length=%" PRIu64
           "\n",
           buffers[n_buffers].start, (uint64_t)buffers[n_buffers].length);
  }

  /* queue buffers */
  printf("queue buffers\n");

  for (i = 0; i < n_buffers; ++i) {
    struct v4l2_buffer buf;

    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
      errno_exit("VIDIOC_QBUF");
  }


  /* query trigger before setting */
  {
    printf("Querying trigger state before configuring\n");
    int tmp;
    int ret;
    ret = ioctl(fd, VIDIOC_G_TRIGGER_SOURCE, &tmp);
    printf("VIDIOC_G_TRIGGER_SOURCE ret: %d, value: %d\n", ret, tmp);
    if(ret < 0)
      return ret;

    ret = ioctl(fd, VIDIOC_G_TRIGGER_ACTIVATION, &tmp);
    printf("VIDIOC_G_TRIGGER_ACTIVATION ret: %d, value: %d\n", ret, tmp);
    if(ret < 0)
      return ret;;
  }


  /* set trigger */
  int trig_ret = 0;
  trig_ret = ioctl(fd, VIDIOC_TRIGGER_MODE_ON);
  printf("VIDIOC_TRIGGER_MODE_ON ret: %d\n", trig_ret);
  if (trig_ret < 0)
    return trig_ret;

  trig_ret = ioctl(fd, VIDIOC_S_TRIGGER_SOURCE, &source);
  printf("VIDIOC_S_TRIGGER_SOURCE ret: %d\n", trig_ret);
  if (trig_ret < 0)
    return trig_ret;

  trig_ret = ioctl(fd, VIDIOC_S_TRIGGER_ACTIVATION, &activation);
  printf("VIDIOC_S_TRIGGER_ACTIVATION ret: %d\n", trig_ret);
  if (trig_ret < 0)
    return trig_ret;

  /* query trigger after setting */
  {
    printf("Querying trigger state after configuring\n");
    int tmp;
    int ret;
    ret = ioctl(fd, VIDIOC_G_TRIGGER_SOURCE, &tmp);
    printf("VIDIOC_G_TRIGGER_SOURCE ret: %d, value: %d\n", ret, tmp);
    if(ret < 0)
      return ret;

    ret = ioctl(fd, VIDIOC_G_TRIGGER_ACTIVATION, &tmp);
    printf("VIDIOC_G_TRIGGER_ACTIVATION ret: %d, value: %d\n", ret, tmp);
    if(ret < 0)
      return ret;
  }

  /* start capture */
  printf("start capture\n");
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
    errno_exit("VIDIOC_STREAMON");

  usleep(2000000);

  if(source == V4L2_TRIGGER_SOURCE_SOFTWARE) {
    printf("Pre-triggering frames (vi driver limitation workaround)\n");
    for (int i = 0; i < req.count-1; i++) {
      printf("start trigger\n");
      if (-1 == ioctl(fd, VIDIOC_TRIGGER_SOFTWARE))
        errno_exit("VIDIOC_TRIGGER_SOFTWARE");

      usleep(2000000);
    }
  } else {
    printf("Not pre-triggering frames (vi driver limitation workaround)\n");
    printf("--> Pre-trigger via hardware %d times, then press return\n", req.count);
    getc(stdin);
  }

// --------------------
//  uint8_t *out_buf = new uint8_t[width*height*3];
//  unsigned char* out_buf = (unsigned char*)malloc( width * height * 3);
// --------------------

  i = 0;
  running = true;

  gettimeofday(&start_time, NULL);
 
  signal(SIGINT, handle_sigint);
  while(running) {
    if(source == V4L2_TRIGGER_SOURCE_SOFTWARE) {
      printf("start trigger with return\n");
      getc(stdin);
      if (-1 == ioctl(fd, VIDIOC_TRIGGER_SOFTWARE))
        errno_exit("VIDIOC_TRIGGER_SOFTWARE");
    } else {
      printf("waiting for external trigger\n");
    }

    while(running) {
      fd_set fds;
      struct timeval tv;
      int r;

      /* dequeue captured buffer */
      CLEAR(buf);
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
        if (errno == EAGAIN) {
          continue;
        }
        errno_exit("VIDIOC_DQBUF");
      }
      assert(buf.index < n_buffers);


      gettimeofday(&end_time, NULL);
 
      seconds  = end_time.tv_sec  - start_time.tv_sec;
      useconds = end_time.tv_usec - start_time.tv_usec;
      milli_time = ((seconds) * 1000 + useconds/1000.0);
  
      printf("frame received %d %ld \n", counter++, milli_time );
      gettimeofday(&start_time, NULL);

      sprintf(filename, "frame%05d.raw", i++);

// -----------------------------------------------------

     save_frame(filename, ((uint8_t*)buffers[buf.index].start), buffers[buf.index].length);
    //  for(int pix = 0; pix < width*height; ++pix) {
    //    memcpy(out_buf + pix*3, ((uint8_t*)buffers[buf.index].start)+4*pix+1, 3);
    //  }
    //  lodepng_encode24_file(filename, out_buf, width, height);
    //  printf("saved frame to %s\n", filename);

// -----------------------------------------------------

      /* queue buffer */
      if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        errno_exit("VIDIOC_QBUF");

      break;
    }
  }

  printf("stop capture\n");
  /* stop capture */
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
    errno_exit("VIDIOC_STREAMOFF");

  printf("unmap buffers\n");
  /* unmap and free buffers */
  for (i = 0; i < n_buffers; ++i)
    if (-1 == munmap(buffers[i].start, buffers[i].length))
      errno_exit("munmap");
  printf("free buffers\n");
  free(buffers);

  printf("close device\n");
  /* close device */
  if (-1 == close(fd))
    errno_exit("close");
  printf("device closed\n");

  // joe: test reopen /dev/video0 again.
  printf("\n\n\n---------- 2nd reopen again -----------\n");
  usleep(1 * 1000 * 1000);

  fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
  if (-1 == fd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n", dev_name, errno,
            strerror(errno));
    exit(EXIT_FAILURE);
  }

  usleep(1 * 1000 * 1000);

  /* close device */
  if (-1 == close(fd))
    errno_exit("close");
  printf("device closed\n");



  fprintf(stderr, "\n");
  return 0;
}
