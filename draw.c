/*
 * drmmodeset - DRM Modesetting Example
 *
 * Written 2014 by Girish Sharma at eInfochips <girish.sharma@einfochips.com>
 *
 * This example is written to run on Qualcomm Snapdragon 600 series board
 * IFC6410 Current IFC6410 boards uses binary blobs for intializing hardware
 * peripherals like DSP, Internal cores, GPU Adreno etc. Android BSP Rel v 1.5,
 * which you need to download from Inforce Techweb. The ZIP file
 * Inforce-IFC6410_AndroidBSP_880160_Rel_Beta_V1.5.zip includes all the
 * proprietary firmware blobs required for IFC6410 to intialize GPU at runtime
 * which should get detected as /dev/dri/card0 by udevfs at boottime.
 *
 * To install the firmware blobs, first locate the file proprietary.tar.gz
 * in the source folder after extracting the Inforce BSP release ZIP file.
 * Then follow the next instructions on how to extract firmware image included
 * in the release
 *
 * $ mkdir image
 * $ sudo tar xzf proprietary.tar.gz -C image/ --strip-components 8
 * proprietary/prebuilt/target/product/msm8960/system/etc/firmware/ $ sudo cp
 * -rf image/firmware /<path-to-ifc6410-rootfs>/lib/ $ rmdir image
 *
 *
 * But it is very generic and run on any embedded device whose kernel support
 * DRM/KMS Also run you can test on your host machine Compilation commands are
 * "gcc -o drmmodeset drmmodeset.c `pkg-config --cflags --libs libdrm` -Wall -O0
 * -g"
 *
 * Dedicated to the Public Domain.
 */

/*
 * DRM Modesetting Howto
 * This document describes the DRM modesetting API. Before we can use the DRM
 * API, we have to include xf86drm.h and xf86drmMode.h. Both are provided by
 * libdrm which every major distribution ships by default. It has no other
 * dependencies and is pretty small.
 *
 * Please ignore all forward-declarations of functions which are used later. I
 * reordered the functions so you can read this document from top to bottom. If
 * you reimplement it, you would probably reorder the functions to avoid all the
 * nasty forward declarations.
 *
 * For easier reading, we ignore all memory-allocation errors of malloc() and
 * friends here. However, we try to correctly handle all other kinds of errors
 * that may occur.
 *
 * All functions and global variables are prefixed with "drmmodeset_*" in this
 * file. So it should be clear whether a function is a local helper or if it is
 * provided by some external library.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct drmmodeset_dev;
static int drmmodeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
                                struct drmmodeset_dev *dev);
static int drmmodeset_create_fb(int fd, struct drmmodeset_dev *dev);
static int drmmodeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
                                struct drmmodeset_dev *dev);
static int drmmodeset_open(int *out, const char *node);
static int drmmodeset_prepare(int fd);
static void drmmodeset_draw(void);
static void drmmodeset_cleanup(int fd);

/*
 * When the linux kernel detects a graphics-card on your machine, it loads the
 * correct device driver (located in kernel-tree at ./drivers/gpu/drm/<xy>) and
 * provides two character-devices to control it. Udev (or whatever hotplugging
 * application you use) will create them as:
 *     /dev/dri/card0
 *     /dev/dri/controlID64
 * We only need the first one. You can hard-code this path into your application
 * like we do here, but it is recommended to use libudev with real hotplugging
 * and multi-seat support. However, this is beyond the scope of this document.
 * Also note that if you have multiple graphics-cards, there may also be
 * /dev/dri/card1, /dev/dri/card2, ...
 *
 * We simply use /dev/dri/card0 here but the user can specify another path on
 * the command line.
 *
 * drmmodeset_open(out, node): This small helper function opens the DRM device
 * which is given as @node. The new fd is stored in @out on success. On failure,
 * a negative error code is returned.
 * After opening the file, we also check for the DRM_CAP_DUMB_BUFFER capability.
 * If the driver supports this capability, we can create simple memory-mapped
 * buffers without any driver-dependent code. As we want to avoid any radeon,
 * nvidia, intel, etc. specific code, we depend on DUMB_BUFFERs here.
 */

static int drmmodeset_open(int *out, const char *node) {
  int fd, ret;
  uint64_t has_dumb;

  fd = open(node, O_RDWR | O_CLOEXEC);
  printf("fb: %d\n", fd);
  if (fd < 0) {
    ret = -errno;
    fprintf(stderr, "cannot open '%s': %m\n", node);
    return ret;
  }

  if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
    fprintf(stderr, "drm device '%s' does not support dumb buffers\n", node);
    close(fd);
    return -EOPNOTSUPP;
  }

  *out = fd;
  return 0;
}

/*
 * As a next step we need to find our available display devices. libdrm provides
 * a drmModeRes structure that contains all the needed information. We can
 * retrieve it via drmModeGetResources(fd) and free it via
 * drmModeFreeResources(res) again.
 *
 * A physical connector on your graphics card is called a "connector". You can
 * plug a monitor into it and control what is displayed. We are definitely
 * interested in what connectors are currently used, so we simply iterate
 * through the list of connectors and try to display a test-picture on each
 * available monitor.
 * However, this isn't as easy as it sounds. First, we need to check whether the
 * connector is actually used (a monitor is plugged in and turned on). Then we
 * need to find a CRTC that can control this connector. CRTCs are described
 * later on. After that we create a framebuffer object. If we have all this, we
 * can mmap() the framebuffer and draw a test-picture into it. Then we can tell
 * the DRM device to show the framebuffer on the given CRTC with the selected
 * connector.
 *
 * As we want to draw moving pictures on the framebuffer, we actually have to
 * remember all these settings. Therefore, we create one "struct drmmodeset_dev"
 * object for each connector+crtc+framebuffer pair that we successfully
 * initialized and push it into the global device-list.
 *
 * Each field of this structure is described when it is first used. But as a
 * summary:
 * "struct drmmodeset_dev" contains: {
 *  - @next: points to the next device in the single-linked list
 *
 *  - @width: width of our buffer object
 *  - @height: height of our buffer object
 *  - @stride: stride value of our buffer object
 *  - @size: size of the memory mapped buffer
 *  - @handle: a DRM handle to the buffer object that we can draw into
 *  - @map: pointer to the memory mapped buffer
 *
 *  - @mode: the display mode that we want to use
 *  - @fb: a framebuffer handle with our buffer object as scanout buffer
 *  - @conn: the connector ID that we want to use with this buffer
 *  - @crtc: the crtc ID that we want to use with this connector
 *  - @saved_crtc: the configuration of the crtc before we changed it. We use it
 *                 so we can restore the same mode when we exit.
 * }
 */

struct drmmodeset_dev {
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t size;
  uint32_t handle;
  uint8_t *map;

  drmModeModeInfo mode;
  uint32_t fb;
  uint32_t dri;
  uint32_t conn;
  uint32_t crtc;
  drmModeCrtc *saved_crtc;
};

static struct drmmodeset_dev *drmmodeset_con = NULL;

/*
 * So as next step we need to actually prepare all connectors that we find. We
 * do this in this little helper function:
 *
 * drmmodeset_prepare(fd): This helper function takes the DRM fd as argument and
 * then simply retrieves the resource-info from the device. It then iterates
 * through all connectors and calls other helper functions to initialize this
 * connector (described later on).
 * If the initialization was successful, we simply add this object as new device
 * into the global drmmodeset device list.
 *
 * The resource-structure contains a list of all connector-IDs. We use the
 * helper function drmModeGetConnector() to retrieve more information on each
 * connector. After we are done with it, we free it again with
 * drmModeFreeConnector().
 * Our helper drmmodeset_setup_dev() returns -ENOENT if the connector is
 * currently unused and no monitor is plugged in. So we can ignore this
 * connector.
 */

static int drmmodeset_prepare(int fd) {
  drmModeRes *res;
  drmModeConnector *conn;
  unsigned int i;
  struct drmmodeset_dev *dev;
  int ret;

  /* retrieve resources */
  res = drmModeGetResources(fd);
  if (!res) {
    fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n", errno);
    return -errno;
  }

  /* iterate all connectors */
  for (i = 0; i < res->count_connectors; ++i) {
    printf("conn %d\n", i);
    /* get information for each connector */
    conn = drmModeGetConnector(fd, res->connectors[i]);
    if (!conn) {
      fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n", i,
              res->connectors[i], errno);
      continue;
    }

    /* create a device structure */
    dev = malloc(sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->conn = conn->connector_id;
    dev->dri = fd;

    /* call helper function to prepare this connector */
    ret = drmmodeset_setup_dev(fd, res, conn, dev);
    if (ret) {
      if (ret != -ENOENT) {
        errno = -ret;
        fprintf(stderr, "cannot setup device for connector %u:%u (%d): %m\n", i,
                res->connectors[i], errno);
      }
      free(dev);
      drmModeFreeConnector(conn);
      continue;
    }

    /* free connector data and link device into global list */
    drmModeFreeConnector(conn);
    drmmodeset_con = dev;
  }

  /* free resources again */
  drmModeFreeResources(res);
  return 0;
}

/*
 * Now we dig deeper into setting up a single connector. As described earlier,
 * we need to check several things first:
 *   * If the connector is currently unused, that is, no monitor is plugged in,
 *     then we can ignore it.
 *   * We have to find a suitable resolution and refresh-rate. All this is
 *     available in drmModeModeInfo structures saved for each crtc. We simply
 *     use the first mode that is available. This is always the mode with the
 *     highest resolution.
 *     A more sophisticated mode-selection should be done in real applications,
 *     though.
 *   * Then we need to find an CRTC that can drive this connector. A CRTC is an
 *     internal resource of each graphics-card. The number of CRTCs controls how
 *     many connectors can be controlled indepedently. That is, a graphics-cards
 *     may have more connectors than CRTCs, which means, not all monitors can be
 *     controlled independently.
 *     There is actually the possibility to control multiple connectors via a
 *     single CRTC if the monitors should display the same content. However, we
 *     do not make use of this here.
 *     So think of connectors as pipelines to the connected monitors and the
 *     CRTCs are the controllers that manage which data goes to which pipeline.
 *     If there are more pipelines than CRTCs, then we cannot control all of
 *     them at the same time.
 *   * We need to create a framebuffer for this connector. A framebuffer is a
 *     memory buffer that we can write XRGB32 data into. So we use this to
 *     render our graphics and then the CRTC can scan-out this data from the
 *     framebuffer onto the monitor.
 */

static int drmmodeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
                                struct drmmodeset_dev *dev) {
  int ret;

  /* check if a monitor is connected */
  if (conn->connection != DRM_MODE_CONNECTED) {
    fprintf(stderr, "ignoring unused connector %u\n", conn->connector_id);
    return -ENOENT;
  }

  /* check if there is at least one valid mode */
  if (conn->count_modes == 0) {
    fprintf(stderr, "no valid mode for connector %u\n", conn->connector_id);
    return -EFAULT;
  }

  /* copy the mode information into our device structure */
  memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
  dev->width = conn->modes[0].hdisplay;
  dev->height = conn->modes[0].vdisplay;
  fprintf(stderr, "mode for connector %u is %ux%u\n", conn->connector_id,
          dev->width, dev->height);

  /* find a crtc for this connector */
  ret = drmmodeset_find_crtc(fd, res, conn, dev);
  if (ret) {
    fprintf(stderr, "no valid crtc for connector %u\n", conn->connector_id);
    return ret;
  }

  /* create a framebuffer for this CRTC */
  ret = drmmodeset_create_fb(fd, dev);
  if (ret) {
    fprintf(stderr, "cannot create framebuffer for connector %u\n",
            conn->connector_id);
    return ret;
  }

  return 0;
}

/*
 * drmmodeset_find_crtc(fd, res, conn, dev): This small helper tries to find a
 * suitable CRTC for the given connector. We have actually have to introduce one
 * more DRM object to make this more clear: Encoders.
 * Encoders help the CRTC to convert data from a framebuffer into the right
 * format that can be used for the chosen connector. We do not have to
 * understand any more of these conversions to make use of it. However, you must
 * know that each connector has a limited list of encoders that it can use. And
 * each encoder can only work with a limited list of CRTCs. So what we do is
 * trying each encoder that is available and looking for a CRTC that this
 * encoder can work with. If we find the first working combination, we are happy
 * and write it into the @dev structure.
 * But before iterating all available encoders, we first try the currently
 * active encoder+crtc on a connector to avoid a full modeset.
 *
 * However, before we can use a CRTC we must make sure that no other device,
 * that we setup previously, is already using this CRTC. Remember, we can only
 * drive one connector per CRTC! So we simply iterate through the
 * "drmmodeset_list" of previously setup devices and check that this CRTC wasn't
 * used before. Otherwise, we continue with the next CRTC/Encoder combination.
 */

static int drmmodeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
                                struct drmmodeset_dev *dev) {
  drmModeEncoder *enc;
  unsigned int i, j;
  int32_t crtc;
  struct drmmodeset_dev *iter;

  /* first try the currently conected encoder+crtc */
  if (conn->encoder_id)
    enc = drmModeGetEncoder(fd, conn->encoder_id);
  else
    enc = NULL;

  if (enc) {
    if (enc->crtc_id) {
      crtc = enc->crtc_id;
      //			for (iter = drmmodeset_list; iter; iter =
      //iter->next) { 				if (iter->crtc == crtc) { 					crtc = -1; 					break;
      //				}
      //			}

      if (crtc >= 0) {
        drmModeFreeEncoder(enc);
        dev->crtc = crtc;
        return 0;
      }
    }

    drmModeFreeEncoder(enc);
  }

  /* If the connector is not currently bound to an encoder or if the
   * encoder+crtc is already used by another connector (actually unlikely
   * but lets be safe), iterate all other available encoders to find a
   * matching CRTC. */
  for (i = 0; i < conn->count_encoders; ++i) {
    enc = drmModeGetEncoder(fd, conn->encoders[i]);
    if (!enc) {
      fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n", i,
              conn->encoders[i], errno);
      continue;
    }

    /* iterate all global CRTCs */
    for (j = 0; j < res->count_crtcs; ++j) {
      /* check whether this CRTC works with the encoder */
      if (!(enc->possible_crtcs & (1 << j)))
        continue;

      /* check that no other device already uses this CRTC */
      crtc = res->crtcs[j];
      //			for (iter = drmmodeset_list; iter; iter =
      //iter->next) { 				if (iter->crtc == crtc) { 					crtc = -1; 					break;
      //				}
      //			}

      /* we have found a CRTC, so save it and return */
      if (crtc >= 0) {
        drmModeFreeEncoder(enc);
        dev->crtc = crtc;
        return 0;
      }
    }

    drmModeFreeEncoder(enc);
  }

  fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
          conn->connector_id);
  return -ENOENT;
}

/*
 * drmmodeset_create_fb(fd, dev): After we have found a crtc+connector+mode
 * combination, we need to actually create a suitable framebuffer that we can
 * use with it. There are actually two ways to do that:
 *   * We can create a so called "dumb buffer". This is a buffer that we can
 *     memory-map via mmap() and every driver supports this. We can use it for
 *     unaccelerated software rendering on the CPU.
 *   * We can use libgbm to create buffers available for hardware-acceleration.
 *     libgbm is an abstraction layer that creates these buffers for each
 *     available DRM driver. As there is no generic API for this, each driver
 *     provides its own way to create these buffers.
 *     We can then use such buffers to create OpenGL contexts with the mesa3D
 *     library.
 * We use the first solution here as it is much simpler and doesn't require any
 * external libraries. However, if you want to use hardware-acceleration via
 * OpenGL, it is actually pretty easy to create such buffers with libgbm and
 * libEGL. But this is beyond the scope of this document.
 *
 * So what we do is requesting a new dumb-buffer from the driver. We specify the
 * same size as the current mode that we selected for the connector.
 * Then we request the driver to prepare this buffer for memory mapping. After
 * that we perform the actual mmap() call. So we can now access the framebuffer
 * memory directly via the dev->map memory map.
 */

static int drmmodeset_create_fb(int fd, struct drmmodeset_dev *dev) {
  struct drm_mode_create_dumb creq;
  struct drm_mode_destroy_dumb dreq;
  struct drm_mode_map_dumb mreq;
  int ret;

  /* create dumb buffer */
  memset(&creq, 0, sizeof(creq));
  creq.width = dev->width;
  creq.height = dev->height;
  creq.bpp = 32;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret < 0) {
    fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
    return -errno;
  }
  dev->stride = creq.pitch;
  dev->size = creq.size;
  dev->handle = creq.handle;

  /* create framebuffer object for the dumb-buffer */
  ret = drmModeAddFB(fd, dev->width, dev->height, 24, 32, dev->stride,
                     dev->handle, &dev->fb);
  if (ret) {
    fprintf(stderr, "cannot create framebuffer (%d): %m\n", errno);
    ret = -errno;
    goto err_destroy;
  }

  /* prepare buffer for memory mapping */
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = dev->handle;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
  if (ret) {
    fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* perform actual memory mapping */
  dev->map =
      mmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
  if (dev->map == MAP_FAILED) {
    fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* clear the framebuffer to 0 */
  memset(dev->map, 0, dev->size);

  return 0;

err_fb:
  drmModeRmFB(fd, dev->fb);
err_destroy:
  memset(&dreq, 0, sizeof(dreq));
  dreq.handle = dev->handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  return ret;
}

/*
 * Finally! We have a connector with a suitable CRTC. We know which mode we want
 * to use and we have a framebuffer of the correct size that we can write to.
 * There is nothing special left to do. We only have to program the CRTC to
 * connect each new framebuffer to each selected connector for each combination
 * that we saved in the global modeset_list.
 * This is done with a call to drmModeSetCrtc().
 *
 * So we are ready for our main() function. First we check whether the user
 * specified a DRM device on the command line, otherwise we use the default
 * /dev/dri/card0. Then we open the device via drmmodeset_open().
 * drmmodeset_prepare() prepares all connectors and we can loop over
 * "drmmodeset_list" and call drmModeSetCrtc() on every CRTC/connector
 * combination.
 *
 * But printing empty black pages is boring so we have another helper function
 * drmmodeset_draw() that draws some colors into the framebuffer for 5 seconds
 * and then returns. And then we have all the cleanup functions which correctly
 * free all devices again after we used them. All these functions are described
 * below the main() function.
 *
 * As a side note: drmModeSetCrtc() actually takes a list of connectors that we
 * want to control with this CRTC. We pass only one connector, though. As
 * explained earlier, if we used multiple connectors, then all connectors would
 * have the same controlling framebuffer so the output would be cloned. This is
 * most often not what you want so we avoid explaining this feature here.
 * Furthermore, all connectors will have to run with the same mode, which is
 * also often not guaranteed. So instead, we only use one connector per CRTC.
 *
 * Before calling drmModeSetCrtc() we also save the current CRTC configuration.
 * This is used in drmmodeset_cleanup() to restore the CRTC to the same mode as
 * was before we changed it. If we don't do this, the screen will stay blank
 * after we exit until another application performs modesetting itself.
 */

// int main(int argc, char **argv)
// {
// 	int ret, fd;
// 	const char *card;
// 	struct drmmodeset_dev *iter;
//
// 	/* check which DRM device to open */
// 	if (argc > 1)
// 		card = argv[1];
// 	else
// 		card = "/dev/dri/card0";
//
// 	fprintf(stderr, "using card '%s'\n", card);
//
// 	/* open the DRM device */
// 	ret = drmmodeset_open(&fd, card);
// 	if (ret)
// 		goto out_return;
//
// 	/* prepare all connectors and CRTCs */
// 	ret = drmmodeset_prepare(fd);
// 	if (ret)
// 		goto out_close;
//
// 	/* perform actual modesetting on each found connector+CRTC */
// 	drmmodeset_con->saved_crtc = drmModeGetCrtc(fd, drmmodeset_con->crtc);
// 	ret = drmModeSetCrtc(fd, drmmodeset_con->crtc, drmmodeset_con->fb, 0, 0,
// 			     &drmmodeset_con->conn, 1, &drmmodeset_con->mode);
// 	if (ret)
// 		fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
// 			drmmodeset_con->conn, errno);
//
// 	/* draw some colors for 5seconds */
// 	drmmodeset_draw();
//
// 	/* cleanup everything */
// 	drmmodeset_cleanup(fd);
//
// 	ret = 0;
//
// out_close:
// 	close(fd);
// out_return:
// 	if (ret) {
// 		errno = -ret;
// 		fprintf(stderr, "drmmodeset failed with error %d: %m\n", errno);
// 	} else {
// 		fprintf(stderr, "exiting\n");
// 	}
// 	return ret;
// }

/*
 * A short helper function to compute a changing color value. No need to
 * understand it.
 */

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod) {
  uint8_t next;

  next = cur + (*up ? 1 : -1) * (rand() % mod);
  if ((*up && next < cur) || (!*up && next > cur)) {
    *up = !*up;
    next = cur;
  }

  return next;
}

/*
 * drmmodeset_draw(): This draws a solid color into all configured framebuffers.
 * Every 100ms the color changes to a slightly different color so we get some
 * kind of smoothly changing color-gradient.
 *
 * The color calculation can be ignored as it is pretty boring. So the
 * interesting stuff is iterating over "drmmodeset_list" and then through all
 * lines and width. We then set each pixel individually to the current color.
 *
 * We do this 50 times as we sleep 100ms after each redraw round. This makes
 * 50*100ms = 5000ms = 5s so it takes about 5seconds to finish this loop.
 *
 * Please note that we draw directly into the framebuffer. This means that you
 * will see flickering as the monitor might refresh while we redraw the screen.
 * To avoid this you would need to use two framebuffers and a call to
 * drmModeSetCrtc() to switch between both buffers.
 * You can also use drmModePageFlip() to do a vsync'ed pageflip. But this is
 * beyond the scope of this document.
 */

static void drmmodeset_draw(void) {
  uint8_t r, g, b;
  bool r_up, g_up, b_up;
  unsigned int i, j, k, off;
  struct drmmodeset_dev *iter;

  iter = drmmodeset_con;

  srand(time(NULL));
  r = rand() % 0xff;
  g = rand() % 0xff;
  b = rand() % 0xff;
  r_up = g_up = b_up = true;

  for (i = 0; i < 50; ++i) {
    r = next_color(&r_up, r, 20);
    g = next_color(&g_up, g, 10);
    b = next_color(&b_up, b, 5);

    for (j = 0; j < iter->height; ++j) {
      for (k = 0; k < iter->width; ++k) {
        off = iter->stride * j + k * 4;
        *(uint32_t *)&iter->map[off] = (r << 16) | (g << 8) | b;
      }
    }

    usleep(100000);
  }
}

/*
 * drmmodeset_cleanup(fd): This cleans up all the devices we created during
 * drmmodeset_prepare(). It resets the CRTCs to their saved states and
 * deallocates all memory. It should be pretty obvious how all of this works.
 */

static void drmmodeset_cleanup(int fd) {
  struct drmmodeset_dev *iter;
  struct drm_mode_destroy_dumb dreq;

  /* remove from global list */
  iter = drmmodeset_con;

  printf("restore\n");
  /* restore saved CRTC configuration */
  drmModeSetCrtc(fd, iter->saved_crtc->crtc_id, iter->saved_crtc->buffer_id,
                 iter->saved_crtc->x, iter->saved_crtc->y, &iter->conn, 1,
                 &iter->saved_crtc->mode);
  drmModeFreeCrtc(iter->saved_crtc);

  /* unmap buffer */
  munmap(iter->map, iter->size);

  /* delete framebuffer */
  drmModeRmFB(fd, iter->fb);

  /* delete dumb buffer */
  memset(&dreq, 0, sizeof(dreq));
  dreq.handle = iter->handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

  /* free allocated memory */
  free(iter);
}

/*
 * I hope this was a short but easy overview of the DRM modesetting API.
 *
 * Understanding how modesetting (as described in this document) works, is
 * essential to understand all further DRM topics.
 *
 * Any feedback is welcome. Feel free to use this code freely for your own
 * documentation or projects.
 *
 *  - Hosted on http://github.com/girish2k44/drmmodeset
 *  - Written by Girish Sharma at eInfochips <girish.sharma@einfochips.com>
 */

#include "draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
// #include <linux/input.h>
// #include <linux/fb.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
// #include <linux/vt.h>
#include <signal.h>
#include <unistd.h>

void image_free(image_t *image) {
  free(image->data);
  image->width = 0;
  image->height = 0;
  image->data = NULL;
  free(image);
}

// Set an individual pixel. This is SLOW for bulk operations.
// Do as little as possible, and memcpy the result.
void set_pixel(int x, int y, context_t *context, int color) {
  int write_index = x + y * context->width;
  if (write_index < context->width * context->height) {
    context->data[x + y * context->width] = color;
  } else {
    printf("Attempted to set color #%x at x=%d, y=%d). (out of bounds)\n",
           color, x, y);
    exit(1);
  }
}

// We scale and crop the image to this new rect.
image_t *scale(image_t *image, int w, int h) {
  int sfx = w / image->width;
  int sfy = h / image->height;
  int crop_x_w = image->width;
  int crop_y_h = image->height;
  int crop_x = 0;
  int crop_y = 0;

  if (sfx < sfy) {
    crop_x_w = image->height * w / h;
    crop_x = (image->width - crop_x_w) / 2;
  } else if (sfx > sfy) {
    crop_y_h = image->width * h / w;
    crop_y = (image->height - crop_y_h) / 2;
  }

  image_t *new_image = malloc(sizeof(image_t));
  new_image->data = malloc(sizeof(int) * w * h);
  new_image->width = w;
  new_image->height = h;
  for (int x = 0; x < w; x++) {
    for (int y = 0; y < h; y++) {
      int tr_x = ((float)crop_x_w / (float)w) * x + crop_x;
      int tr_y = ((float)crop_y_h / (float)h) * y + crop_y;
      new_image->data[y * w + x] = image->data[tr_y * image->width + tr_x];
    }
  }

  return new_image;
  /*

  for(int x = 0; x < w; x++) {
      for(int y = 0; y < h; y++) {
          int tr_x = ((float) image->width / (float) w) * (float) x;
          int tr_y = ((float) image->height / (float) h) * (float) y;
          new_image->data[y * w + x] = image->data[tr_y * image->width + tr_x];
      }
  }
*/
}

// !! This operation is potentially unsafe. Use drawImage. It's harder to mess
// up. X and w are the size of the array.
void draw_array(int x, int y, int w, int h, int *array, context_t *context) {
  // Ignore draws out of bounds
  if (x > context->width || y > context->height) {
    return;
  }

  // Ignore draws out of bounds
  if (x + w < 0 || y + h < 0) {
    return;
  }

  // Column and row correction for partial onscreen images
  int cy = 0;
  int cx = 0;

  // if y is less than 0, trim that many lines off the render.
  if (y < 0) {
    cy = -y;
  }

  // If x is less than 0, trim that many pixels off the render line.
  if (x < 0) {
    cx = -x;
  }

  // Number of items in a line
  int line_width = (w - cx);

  // Number of lines total.
  // We don't subtract cy because the loop starts with cy already advanced.
  int line_count = h;

  // If the end of the line goes offscreen, trim that many pixels off the
  // row.
  if (x + w > context->width) {
    line_width -= ((x + w) - context->width);
  }

  // If the number of rows is more than the height of the context, trim
  // them off.
  if (y + h > context->height) {
    line_count -= ((y + h) - context->height);
  }

  for (; cy < line_count; cy++) {
    // Draw each graphics line.
    memcpy(&context->data[context->width * y + context->width * cy + x + cx],
           &array[cy * w] + cx, sizeof(int) * line_width);
  }
}

void draw_image(int x, int y, image_t *image, context_t *context) {
  draw_array(x, y, image->width, image->height, image->data, context);
}

void draw_rect(int x, int y, int w, int h, context_t *context, int color) {
  // Ignore draws out of bounds
  if (x > context->width || y > context->height) {
    return;
  }

  // Ignore draws out of bounds
  if (x + w < 0 || y + h < 0) {
    return;
  }
  // Trim offscreen pixels
  if (x < 0) {
    w += x;
    x = 0;
  }

  // Trim offscreen lines
  if (y < 0) {
    h += y;
    y = 0;
  }

  // Trim offscreen pixels
  if (x + w > context->width) {
    w -= ((x + w) - context->width);
  }

  // Trim offscreen lines.
  if (y + h > context->height) {
    h -= ((y + h) - context->height);
  }

  // Set the first line.
  for (int rx = x; rx < x + w; rx++) {
    set_pixel(rx, y, context, color);
  }

  // Repeat the first line.
  for (int ry = 1; ry < h; ry++) {
    memcpy(&context->data[context->width * y + context->width * ry + x],
           &context->data[context->width * y + x], w * sizeof(int));
  }
}

void clear_context_color(context_t *context, int color) {
  draw_rect(0, 0, context->width, context->height, context, color);
}

void clear_context(context_t *context) {
  memset(context->data, 0, context->width * context->height * sizeof(int));
}

void test_pattern(context_t *context) {
  const unsigned int pattern[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                   0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};

  unsigned columnWidth = context->width / 8;
  for (int rx = 0; rx < context->width; rx++) {
    set_pixel(rx, 0, context, pattern[rx / columnWidth]);
  }

  // make it faster: memcpy the first row.
  for (int y = 1; y < context->height; y++) {
    memcpy(&context[context->width * y], context, context->width * sizeof(int));
  }
}

void context_release(context_t *context) {
  printf("fb: %d\n", drmmodeset_con->dri);
  drmmodeset_cleanup(drmmodeset_con->dri);
  close(context->fb_file_desc);
  context->data = NULL;
  context->fb_file_desc = 0;
  free(context);
}

context_t *context_create() {
  //     char *FB_NAME = "/dev/fb0";
  //     void* mapped_ptr = NULL;
  //     struct fb_fix_screeninfo fb_fixinfo;
  //     struct fb_var_screeninfo fb_varinfo;
  //     int fb_file_desc;
  //     int fb_size = 0;
  //
  //     // Open the framebuffer device in read write
  //     fb_file_desc = open(FB_NAME, O_RDWR);
  //     if (fb_file_desc < 0) {
  //         printf("Unable to open %s.\n", FB_NAME);
  //         return NULL;
  //     }
  //     //Do Ioctl. Retrieve fixed screen info.
  //     if (ioctl(fb_file_desc, FBIOGET_FSCREENINFO, &fb_fixinfo) < 0) {
  //         printf("get fixed screen info failed: %s\n",
  //                strerror(errno));
  //         close(fb_file_desc);
  //         return NULL;
  //     }
  //     // Do Ioctl. Get the variable screen info.
  //     if (ioctl(fb_file_desc, FBIOGET_VSCREENINFO, &fb_varinfo) < 0) {
  //         printf("Unable to retrieve variable screen info: %s\n",
  //                strerror(errno));
  //         close(fb_file_desc);
  //         return NULL;
  //     }
  //
  //     // Calculate the size to mmap
  //     fb_size = fb_fixinfo.line_length * fb_varinfo.yres;
  //
  //     // Now mmap the framebuffer.
  //     mapped_ptr = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
  //     fb_file_desc,0);
  //
  //     if (mapped_ptr == NULL) {
  //         printf("mmap failed:\n");
  //         close(fb_file_desc);
  //         return NULL;
  //     }
  //
  //     context_t * context = malloc(sizeof(context_t));
  //     context->data = (int *) mapped_ptr;
  //     context->width = fb_fixinfo.line_length / 4;
  //     context->height = fb_varinfo.yres;
  //     context->fb_file_desc = fb_file_desc;
  //     context->fb_name = FB_NAME;
  //    return context;
  int ret, fd;
  const char *card;
  struct drmmodeset_dev *iter;

  /* check which DRM device to open */
  card = "/dev/dri/card0";

  fprintf(stderr, "using card '%s'\n", card);

  /* open the DRM device */
  ret = drmmodeset_open(&fd, card);
  if (ret) {
    card = "/dev/dri/card1"; // fallback
    ret = drmmodeset_open(&fd, card);
    if (ret)
      goto out_return;
  }

  /* prepare all connectors and CRTCs */
  ret = drmmodeset_prepare(fd);
  if (ret) {
    close(fd);
    goto out_return;
  }

  /* perform actual modesetting on each found connector+CRTC */
  drmmodeset_con->saved_crtc = drmModeGetCrtc(fd, drmmodeset_con->crtc);
  ret = drmModeSetCrtc(fd, drmmodeset_con->crtc, drmmodeset_con->fb, 0, 0,
                       &drmmodeset_con->conn, 1, &drmmodeset_con->mode);
  if (ret)
    fprintf(stderr, "cannot set CRTC for connector %u (%d): %m\n",
            drmmodeset_con->conn, errno);

  // 	/* draw some colors for 5seconds */
  // 	drmmodeset_draw();
  //
  // 	/* cleanup everything */
  // 	drmmodeset_cleanup(fd);

  ret = 0;

out_return:
  if (ret) {
    errno = -ret;
    fprintf(stderr, "drmmodeset failed with error %d: %m\n", errno);
    return NULL;
  }

  context_t *context = malloc(sizeof(context_t));
  context->data = (int *)drmmodeset_con->map;
  context->width = drmmodeset_con->width;
  context->height = drmmodeset_con->height;
  context->fb_file_desc = drmmodeset_con->fb;
  context->fb_name = card;
  return context;
}
