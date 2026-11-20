#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void report(int fd, const char *label) {
  struct fb_fix_screeninfo fix;
  struct fb_var_screeninfo var;
  if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
      ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
    perror("FBIOGET");
    exit(1);
  }
  printf("%s: visible=%ux%u virtual=%ux%u bpp=%u stride=%u smem=%u yoffset=%u "
         "RGB=%u/%u,%u/%u,%u/%u,A=%u/%u\n",
         label, var.xres, var.yres, var.xres_virtual, var.yres_virtual,
         var.bits_per_pixel, fix.line_length, fix.smem_len, var.yoffset,
         var.red.offset, var.red.length, var.green.offset, var.green.length,
         var.blue.offset, var.blue.length, var.transp.offset, var.transp.length);
}

int main(int argc, char **argv) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    perror("/dev/fb0");
    return 1;
  }
  report(fd, "before");
  if (argc == 3) {
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
      perror("FBIOGET_VSCREENINFO");
      return 1;
    }
    var.xres = var.xres_virtual = (unsigned)strtoul(argv[1], NULL, 10);
    var.yres = (unsigned)strtoul(argv[2], NULL, 10);
    var.yres_virtual = var.yres;
    var.xoffset = var.yoffset = 0;
    var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0) {
      fprintf(stderr, "FBIOPUT_VSCREENINFO: %s\n", strerror(errno));
      close(fd);
      return 2;
    }
    report(fd, "after");
  }
  close(fd);
  return 0;
}
