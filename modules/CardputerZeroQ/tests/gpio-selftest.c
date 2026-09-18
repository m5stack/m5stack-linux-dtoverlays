/* Build: aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -pthread -o gpio-selftest gpio-selftest.c */
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/gpio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LED_ITERATIONS 100

struct led_test {
    int chip;
    unsigned int pin;
    int initial;
    int failed;
};

static int request_line(int chip, unsigned int pin, uint64_t flags, int value)
{
    struct gpio_v2_line_request req = {0};

    req.offsets[0] = pin;
    req.num_lines = 1;
    req.config.flags = flags;
    snprintf(req.consumer, sizeof(req.consumer), "m5io-selftest");
    if (flags & GPIO_V2_LINE_FLAG_OUTPUT) {
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = !!value;
        req.config.attrs[0].mask = 1;
    }
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
        return -1;
    return req.fd;
}

static int get_value(int fd)
{
    struct gpio_v2_line_values values = {.mask = 1};

    if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values) < 0)
        return -1;
    return !!(values.bits & 1);
}

static int set_value(int fd, int value)
{
    struct gpio_v2_line_values values = {.mask = 1, .bits = !!value};

    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

static void *test_led(void *arg)
{
    struct led_test *test = arg;
    int fd, i, actual;

    test->failed = 0;
    fd = request_line(test->chip, test->pin, GPIO_V2_LINE_FLAG_OUTPUT,
                      test->initial);
    if (fd < 0) {
        perror("request LED output");
        test->failed = 1;
        return NULL;
    }
    for (i = 0; i < LED_ITERATIONS; i++) {
        if (set_value(fd, i & 1) < 0) {
            perror("set LED");
            test->failed = 1;
            break;
        }
        actual = get_value(fd);
        if (actual != (i & 1)) {
            fprintf(stderr, "GPIO %u iteration %d: expected %d, read %d\n",
                    test->pin, i, i & 1, actual);
            test->failed = 1;
            break;
        }
    }
    if (set_value(fd, test->initial) < 0 || get_value(fd) != test->initial) {
        fprintf(stderr, "GPIO %u: failed to restore level %d\n",
                test->pin, test->initial);
        test->failed = 1;
    }
    close(fd);
    printf("LED%u: %s (%d set/read pairs, restored %d)\n", test->pin - 2,
           test->failed ? "FAIL" : "PASS", i, test->initial);
    return NULL;
}

static int test_rejected_mode(int chip, unsigned int pin, uint64_t flags)
{
    int fd = request_line(chip, pin, flags, 0);
    int err = errno;

    if (fd >= 0) {
        close(fd);
        fprintf(stderr, "GPIO %u unexpectedly accepted unsupported mode\n", pin);
        return 1;
    }
    if (err != EINVAL && err != EOPNOTSUPP) {
        fprintf(stderr, "GPIO %u: unexpected failure: %s\n", pin, strerror(err));
        return 1;
    }
    printf("GPIO %u unsupported mode: PASS (%s)\n", pin, strerror(err));
    return 0;
}

static int find_chip(void)
{
    struct gpiochip_info info;
    glob_t paths = {0};
    size_t i;
    int fd = -1;

    if (glob("/dev/gpiochip*", 0, NULL, &paths)) {
        globfree(&paths);
        return -1;
    }
    for (i = 0; i < paths.gl_pathc; i++) {
        fd = open(paths.gl_pathv[i], O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (!ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) &&
            !strcmp(info.label, "m5io-hub-gpio")) {
            printf("Chip: %s label=%s lines=%u\n", paths.gl_pathv[i],
                   info.label, info.lines);
            if (info.lines == 17)
                break;
        }
        close(fd);
        fd = -1;
    }
    globfree(&paths);
    return fd;
}

int main(void)
{
    struct led_test leds[3] = {0};
    pthread_t threads[3];
    int levels[17], chip, fd, i, started, failed = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    chip = find_chip();
    if (chip < 0) {
        fprintf(stderr, "No accessible 17-line m5io-hub-gpio chip\n");
        return 1;
    }
    /* No direction flags: sample current levels without changing pin modes. */
    for (i = 0; i < 17; i++) {
        fd = request_line(chip, i, 0, 0);
        if (fd < 0) {
            perror("request line as-is");
            close(chip);
            return 1;
        }
        levels[i] = get_value(fd);
        close(fd);
        if (levels[i] < 0) {
            perror("read baseline");
            close(chip);
            return 1;
        }
        printf("Baseline GPIO %d = %d\n", i, levels[i]);
    }
    puts("Sequential LED test");
    for (i = 0; i < 3; i++) {
        leds[i].chip = chip;
        leds[i].pin = i + 3;
        leds[i].initial = levels[i + 3];
        test_led(&leds[i]);
        failed |= leds[i].failed;
    }
    puts("Concurrent LED test");
    for (started = 0; started < 3; started++) {
        int err = pthread_create(&threads[started], NULL, test_led, &leds[started]);
        if (err) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            failed = 1;
            break;
        }
    }
    for (i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
        failed |= leds[i].failed;
    }
    failed |= test_rejected_mode(chip, 3, GPIO_V2_LINE_FLAG_INPUT);
    failed |= test_rejected_mode(chip, 6, GPIO_V2_LINE_FLAG_OUTPUT);
    fd = request_line(chip, 6, GPIO_V2_LINE_FLAG_INPUT, 0);
    if (fd < 0) {
        perror("request SD_DET input");
        failed = 1;
    } else {
        int level = get_value(fd);
        printf("SD_DET input: %s (level=%d)\n", level < 0 ? "FAIL" : "PASS", level);
        failed |= level < 0;
        close(fd);
    }
    close(chip);
    puts(failed ? "RESULT: FAIL" : "RESULT: PASS");
    return failed ? 1 : 0;
}
