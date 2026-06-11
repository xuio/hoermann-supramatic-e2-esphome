#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "hcp2_engine.h"

#define COMMAND_BUFFER_LEN 128

typedef struct {
  int serial_fd;
} responder_port_t;

static uint32_t now_us(void *user) {
  (void) user;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t) ((uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u);
}

static void tx_all(void *user, const uint8_t *data, uint8_t len) {
  responder_port_t *port = (responder_port_t *) user;
  uint8_t offset = 0;
  while (offset < len) {
    ssize_t written = write(port->serial_fd, data + offset, (size_t) (len - offset));
    if (written > 0) {
      offset = (uint8_t) (offset + written);
      continue;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    perror("host_responder write");
    exit(2);
  }
}

static void de_set(void *user, uint8_t enabled) {
  (void) user;
  (void) enabled;
}

static int parse_uint_arg(const char *arg, unsigned long *out) {
  char *end = NULL;
  unsigned long value;
  if (arg == NULL || *arg == '\0') {
    return 0;
  }
  value = strtoul(arg, &end, 0);
  if (end == arg || *end != '\0') {
    return 0;
  }
  *out = value;
  return 1;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + c - 'a';
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  }
  return -1;
}

static int parse_hex_exact(const char *hex, uint8_t *out, size_t expected_len) {
  size_t len = 0;
  while (*hex != '\0') {
    int hi;
    int lo;
    while (*hex == ' ' || *hex == '\t' || *hex == '\r' || *hex == '\n') {
      hex++;
    }
    if (*hex == '\0') {
      break;
    }
    hi = hex_value(hex[0]);
    lo = hex_value(hex[1]);
    if (hi < 0 || lo < 0 || len >= expected_len) {
      return 0;
    }
    out[len++] = (uint8_t) ((hi << 4) | lo);
    hex += 2;
  }
  return len == expected_len;
}

static hcp2_button_t parse_button(const char *name) {
  if (strcmp(name, "open") == 0) {
    return HCP2_BUTTON_OPEN;
  }
  if (strcmp(name, "close") == 0) {
    return HCP2_BUTTON_CLOSE;
  }
  if (strcmp(name, "stop") == 0) {
    return HCP2_BUTTON_STOP;
  }
  if (strcmp(name, "vent") == 0) {
    return HCP2_BUTTON_VENT;
  }
  if (strcmp(name, "half") == 0) {
    return HCP2_BUTTON_HALF;
  }
  if (strcmp(name, "light") == 0) {
    return HCP2_BUTTON_LIGHT;
  }
  return HCP2_BUTTON_NONE;
}

static int configure_serial(int fd) {
  struct termios tio;
  if (tcgetattr(fd, &tio) != 0) {
    return -1;
  }
  cfmakeraw(&tio);
  tio.c_cflag |= (CLOCAL | CREAD);
#ifdef CRTSCTS
  tio.c_cflag &= ~CRTSCTS;
#endif
  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    return -1;
  }
  return 0;
}

static int open_serial(const char *path) {
  int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }
  if (configure_serial(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int use_fd(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return -1;
  }
  return fd;
}

static void handle_command(hcp2_engine_t *engine, hcp2_engine_config_t *config, responder_port_t *port, char *line) {
  char *cmd;
  char *arg;
  char *saveptr = NULL;

  cmd = strtok_r(line, " \t\r\n", &saveptr);
  arg = strtok_r(NULL, "\r\n", &saveptr);
  if (cmd == NULL) {
    return;
  }

  if (strcmp(cmd, "press") == 0 && arg != NULL) {
    char *button_name = strtok(arg, " \t\r\n");
    hcp2_button_t button = button_name != NULL ? parse_button(button_name) : HCP2_BUTTON_NONE;
    if (button == HCP2_BUTTON_NONE) {
      printf("ERR unknown-button\n");
    } else if (hcp2_engine_press_button(engine, button)) {
      printf("OK press %s\n", button_name);
    } else {
      printf("ERR busy\n");
    }
    fflush(stdout);
    return;
  }

  if (strcmp(cmd, "set") == 0 && arg != NULL) {
    char *field = strtok(arg, " \t\r\n");
    char *value = strtok(NULL, "\r\n");
    if (field != NULL && value != NULL && strcmp(field, "signature") == 0 &&
        parse_hex_exact(value, config->signature, HCP2_SIGNATURE_LEN)) {
      hcp2_engine_init(engine, &(hcp2_port_t){port, now_us, tx_all, de_set}, config);
      printf("OK set signature\n");
    } else {
      printf("ERR bad-set\n");
    }
    fflush(stdout);
    return;
  }

  if (strcmp(cmd, "quit") == 0) {
    printf("OK quit\n");
    fflush(stdout);
    exit(0);
  }

  printf("ERR unknown-command\n");
  fflush(stdout);
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s (--device PATH | --fd FD) [--slave-id N] [--response-delay-us N] [--button-press-us N]\n",
          argv0);
}

int main(int argc, char **argv) {
  const char *device = NULL;
  int inherited_fd = -1;
  unsigned long value;
  hcp2_engine_config_t config;
  responder_port_t responder_port;
  hcp2_port_t port;
  hcp2_engine_t engine;
  char command_buffer[COMMAND_BUFFER_LEN];
  size_t command_len = 0;
  int stdin_flags;
  int i;

  hcp2_engine_config_default(&config);

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      device = argv[++i];
    } else if (strcmp(argv[i], "--fd") == 0 && i + 1 < argc && parse_uint_arg(argv[++i], &value)) {
      inherited_fd = (int) value;
    } else if (strcmp(argv[i], "--slave-id") == 0 && i + 1 < argc && parse_uint_arg(argv[++i], &value) &&
               value > 0 && value <= 247) {
      config.slave_id = (uint8_t) value;
    } else if (strcmp(argv[i], "--response-delay-us") == 0 && i + 1 < argc && parse_uint_arg(argv[++i], &value)) {
      config.response_delay_us = (uint32_t) value;
    } else if (strcmp(argv[i], "--button-press-us") == 0 && i + 1 < argc && parse_uint_arg(argv[++i], &value)) {
      config.button_press_us = (uint32_t) value;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if ((device == NULL && inherited_fd < 0) || (device != NULL && inherited_fd >= 0)) {
    usage(argv[0]);
    return 2;
  }

  responder_port.serial_fd = inherited_fd >= 0 ? use_fd(inherited_fd) : open_serial(device);
  if (responder_port.serial_fd < 0) {
    perror("host_responder open_serial_or_fd");
    return 2;
  }

  stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (stdin_flags >= 0) {
    fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
  }

  port.user = &responder_port;
  port.now_us = now_us;
  port.tx = tx_all;
  port.de_set = de_set;
  hcp2_engine_init(&engine, &port, &config);

  printf("READY host_responder slave_id=%u\n", config.slave_id);
  fflush(stdout);

  while (1) {
    fd_set readfds;
    struct timeval timeout;
    int max_fd = responder_port.serial_fd > STDIN_FILENO ? responder_port.serial_fd : STDIN_FILENO;
    int selected;

    hcp2_engine_poll(&engine);

    FD_ZERO(&readfds);
    FD_SET(responder_port.serial_fd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 500;

    selected = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
    if (selected < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("host_responder select");
      return 2;
    }

    if (FD_ISSET(responder_port.serial_fd, &readfds)) {
      uint8_t data[64];
      ssize_t read_len;
      while ((read_len = read(responder_port.serial_fd, data, sizeof(data))) > 0) {
        ssize_t j;
        for (j = 0; j < read_len; j++) {
          hcp2_engine_rx_byte(&engine, data[j], HCP2_RX_OK);
        }
      }
      if (read_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        perror("host_responder serial read");
        return 2;
      }
    }

    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      char input[64];
      ssize_t read_len;
      while ((read_len = read(STDIN_FILENO, input, sizeof(input))) > 0) {
        ssize_t j;
        for (j = 0; j < read_len; j++) {
          if (input[j] == '\n') {
            command_buffer[command_len] = '\0';
            handle_command(&engine, &config, &responder_port, command_buffer);
            command_len = 0;
          } else if (command_len < sizeof(command_buffer) - 1u) {
            command_buffer[command_len++] = input[j];
          } else {
            command_len = 0;
            printf("ERR command-too-long\n");
            fflush(stdout);
          }
        }
      }
      if (read_len == 0) {
        return 0;
      }
      if (read_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        perror("host_responder stdin read");
        return 2;
      }
    }
  }
}
