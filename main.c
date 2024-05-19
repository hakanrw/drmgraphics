#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <string.h>
#include <termios.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>

#include <linux/input.h>
#include <linux/fb.h>
#include <linux/vt.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/kd.h>

#include "font.h"
#include "draw.h"
#include "img-png.h"
#include "img-jpeg.h"

int runflag = 1;
int ttyfd = -1;

struct termios old_tio;

void set_noncanonical_nonblocking_mode(struct termios *old_tio) {
    struct termios new_tio;

    new_tio = *old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void restore_terminal_mode(struct termios *old_tio) {
    tcsetattr(STDIN_FILENO, TCSANOW, old_tio);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}

// Intercept SIGINT
void sig_handler(int signo) {
    if (signo == SIGINT) {
        printf("SIGINT\n");
        runflag = 0;
    }

    // If we segfault in graphics mode, we can't get out.
    if (signo == SIGSEGV) {
        if (ttyfd == -1) {
            printf("[!] Error: could not open the tty\n");
        } else {
            ioctl(ttyfd, KDSETMODE, KD_TEXT);
        }

        printf("Segmentation Fault.\n");
        restore_terminal_mode(&old_tio);

        exit(1);
    }
}

int main() {
    // Intercept SIGINT so we can shut down graphics loops.
    if (signal(SIGINT, sig_handler) == SIG_ERR) {
         printf("\ncan't catch SIGINT\n");
    }

    if (signal(SIGSEGV, sig_handler) == SIG_ERR) {
        printf("\ncan't catch SIGSEGV\n");
    }

    tcgetattr(STDIN_FILENO, &old_tio);
    set_noncanonical_nonblocking_mode(&old_tio);

    context_t * context = context_create();
    fontmap_t * fontmap = fontmap_default();
    printf("[+] Graphics Context: 0x%x\n", context);

    // Attempt to set graphics mode
   	// This line enables graphics mode on the tty.
    if (ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS) == -1) {
    	printf("[!] Error: could not set KD_GRAPHICS\n");
		return 1;
    }

    int count = 0;
    int colors[] = {0xFFFF00, 0xFF0000, 0x00FF00, 0x0000FF, 0x00FFFF};
    int color_size = 5;


    if(context != NULL){
//        image_t * jpegImage = read_jpeg_file("./nyc.jpg");
//        image_t * scaledBackgroundImage = scale(jpegImage, context->width, context->height);
        char buf[256] = "Ego in the houseee gimme the musicc";

//        clear_context(context);
//        draw_image(0, 0, scaledBackgroundImage, context);
//        draw_rect(-100, -100, 200, 200, context, 0xFF0000);
//        draw_rect(context->width - 100, context->height - 100, 200, 200, context, 0xFFFF00);
//        draw_rect(context->width - 100, -100, 200, 200, context, 0x00FF00);
//        draw_rect(-100, context->height - 100, 200, 200, context, 0x0000FF);
//        draw_rect(context->width / 2 - 200, context->height / 2 - 200, 400, 400, context, 0x00FFFF);

        int time_ms = 0;

        while (runflag) {
            clear_context(context);
            draw_rect(-100, -100, 200, 200, context, colors[count]);
            draw_rect(context->width - 100, context->height - 100, 200, 200, context, colors[(count + 1) % color_size]);
            draw_rect(context->width - 100, -100, 200, 200, context, colors[(count + 2) % color_size]);
            draw_rect(-100, context->height - 100, 200, 200, context, colors[(count + 3) % color_size]);
            draw_rect(context->width / 2 - 200, context->height / 2 - 200, 400, 400, context, colors[(count + 4) % color_size]);
//            draw_string(200, 200, buf, fontmap, context);
            int val = getchar();
            const char concstr[2] = { val, 0 };

            // we got a keypress
            if (val != -1) {
                if (val == 127) buf[strlen(buf) - 1] = 0;
                else strcat(buf, concstr);
            }

            // draw the text, break it into line strings.
            char bufcpy[256];
            strcpy(bufcpy, buf);

            char *line = strtok(bufcpy, "\n");
            int idx = 0;

            while (line != NULL) {
                draw_string(200, 200 + idx * 30, line, fontmap, context);
                line = strtok(NULL, "\n");
                idx++;
            }

            usleep(20 * 1000);
            time_ms += 20;
            time_ms = time_ms % 1000;

            if (time_ms == 0) {
                ++count;
                count = count % color_size;
            }
        }

//        image_free(jpegImage);
//        image_free(scaledBackgroundImage);
        fontmap_free(fontmap);
        context_release(context);
    }

    ioctl(STDIN_FILENO, KDSETMODE, KD_TEXT);
    restore_terminal_mode(&old_tio);

    printf("[+] Shutdown successful.\n");
    return 0;
}

