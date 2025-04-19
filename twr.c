#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>

#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <glia/glia.h>
#include <runara/runara.h>

typedef struct {
  char* buf;
  size_t buflen;
  int32_t masterfd;
  struct termios prevterm;
  pthread_t ptythread;
} pty_data_t;

typedef struct {
  RnState* rn;
} state_t;

static pty_data_t* s_pty;
static state_t s;

#define BUF_SIZE 65536

void cleanup() {
  tcsetattr(STDIN_FILENO, TCSANOW, &s_pty->prevterm);
  close(s_pty->masterfd);
  pthread_cancel(s_pty->ptythread);  
  pthread_join(s_pty->ptythread, NULL);  
  free(s_pty->buf);
  free(s_pty);
}

void sigint_handler(int sig) {
  printf("SIGINT.\n");
  if (s_pty) {
    cleanup();
  }
  exit(0);  
}

void* ptyhandler(void* data) {
  pty_data_t* pty = (pty_data_t*)data;
  fd_set fds;
  char buf[1024];
  while (1) {
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(pty->masterfd, &fds);
    int nfds = (STDIN_FILENO > pty->masterfd ? STDIN_FILENO : pty->masterfd) + 1;

    if (select(nfds, &fds, NULL, NULL, NULL) < 0) {
      perror("select");
      break;
    }
    if (FD_ISSET(pty->masterfd, &fds)) {
      ssize_t n = read(pty->masterfd, buf, sizeof(buf));
      if (n <= 0) break;

      if(pty->buflen + n < BUF_SIZE) {
        memcpy(pty->buf + pty->buflen, buf, n);
        pty->buflen += n;
      }
    }
  }

  return NULL;
}

void resizecb(GLFWwindow* window, int w, int h) {
  //rn_resize_display(s.rn, w, h);
}

pty_data_t* setuppty(void) {
  pty_data_t* data = malloc(sizeof(*data));
  data->buf = malloc(BUF_SIZE);
  data->buflen = 0;

  pid_t pid;

  tcgetattr(STDIN_FILENO, &data->prevterm); 
  struct termios raw = data->prevterm;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  pid = forkpty(&data->masterfd, NULL, NULL, NULL);
  if (pid == -1) {
    fprintf(stderr, "twr: failed to call forkpty().\n");
    perror("forkpty");
    return NULL;
  }

  if (pid == 0) {
    // Child: replace with shell
    execlp("bash", "bash", (char *)NULL);
    perror("execlp");
    fprintf(stderr, "twr: failed to call execlp() with bash.\n");
    perror("execlp");
    return NULL;
  }

  return data;
}
int main() {
  if(!glfwInit()) {
    fprintf(stderr, "twr: failed to initialize GLFW.\n");
  }
  // Set up signal handler for Ctrl+C (SIGINT)
  signal(SIGINT, sigint_handler);

  s_pty = setuppty();
  if(!s_pty) return 1;

  if (pthread_create(&s_pty->ptythread, NULL, ptyhandler, (void *)s_pty) != 0) {
    fprintf(stderr, "twr: pty: error creating pty thread\n");
    return 1;
  }

  GLFWwindow* window = glfwCreateWindow(1280, 720, "twr alhpa", NULL, NULL);

  glfwSetFramebufferSizeCallback(window, resizecb);

  glfwMakeContextCurrent(window);


  while(!glfwWindowShouldClose(window)) {
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glfwPollEvents();
    glfwSwapBuffers(window);
  }

  cleanup();

  return 0;
}

