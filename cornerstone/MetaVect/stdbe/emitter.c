#include "../internal.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MV_NUMBER_CAPACITY 64

typedef struct {
    mv_output output;
    size_t capacity;
    mv_status status;
} mv_buffer;

static void append(mv_buffer *buffer, const char *format, ...)
{
    /* Each append describes at most one element or a fixed prologue. */
    char text[1024];
    va_list args;
    int length;
    size_t needed, capacity;

    if (buffer->status != MV_STATUS_OK)
        return;
    va_start(args, format);
    length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length < 0) {
        buffer->status = MV_STATUS_BACKEND_ERROR;
        return;
    }
    if ((size_t)length >= sizeof text ||
        (size_t)length >= SIZE_MAX - buffer->output.size) {
        buffer->status = MV_STATUS_LIMIT_EXCEEDED;
        return;
    }

    needed = buffer->output.size + (size_t)length + 1;
    if (needed > buffer->capacity) {
        char *data;

        capacity = buffer->capacity ? buffer->capacity : 512;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        data = realloc(buffer->output.data, capacity);
        if (!data) {
            buffer->status = MV_STATUS_OUT_OF_MEMORY;
            return;
        }
        buffer->output.data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->output.data + buffer->output.size, text, (size_t)length + 1);
    buffer->output.size += (size_t)length;
}

static int eps_number_in_range(double value)
{
    /* Portable normal real operands; don't rely on support for subnormals. */
    const double min = 1.1754943508222875e-38;
    return value == 0 || (value >= min && value <= 1e38) ||
           (value <= -min && value >= -1e38);
}

static int format_number(char text[MV_NUMBER_CAPACITY], double value, int eps)
{
    int length = mv_format_number(text, MV_NUMBER_CAPACITY, value);
    if (length < 0 || length >= MV_NUMBER_CAPACITY)
        return 0;
    if (eps) {
        char *exponent = strchr(text, 'e');
        if (exponent)
            *exponent = 'E';
    }
    return 1;
}

/* Use decimal, not scientific notation, for the DSC bounding-box comment. */
static int format_hires_bound(char out[MV_NUMBER_CAPACITY], const char *text)
{
    const char *exponent = strpbrk(text, "eE");
    char digits[MV_NUMBER_CAPACITY];
    size_t count = 0;
    long point = 0;
    int fraction = 0;

    if (!exponent) {
        strcpy(out, text);
        return 1;
    }
    /* Canvas dimensions are positive and the exponent is ASCII from %g. */
    for (; text < exponent; ++text) {
        if (*text == '.') {
            fraction = 1;
        } else {
            digits[count++] = *text;
            if (!fraction)
                ++point;
        }
    }
    point += strtol(exponent + 1, NULL, 10);
    if (point <= 0) {
        size_t zeros = (size_t)-point;

        if (zeros + count + 3 > MV_NUMBER_CAPACITY)
            return 0;
        *out++ = '0';
        *out++ = '.';
        memset(out, '0', zeros);
        out += zeros;
        memcpy(out, digits, count);
        out += count;
    } else if ((size_t)point < count) {
        if (count + 2 > MV_NUMBER_CAPACITY)
            return 0;
        memcpy(out, digits, (size_t)point);
        out += point;
        *out++ = '.';
        memcpy(out, digits + point, count - (size_t)point);
        out += count - (size_t)point;
    } else {
        if ((size_t)point + 1 > MV_NUMBER_CAPACITY)
            return 0;
        memcpy(out, digits, count);
        memset(out + count, '0', (size_t)point - count);
        out += point;
    }
    *out = '\0';
    return 1;
}

static void svg_begin(mv_buffer *buffer, const char *width, const char *height)
{
    append(buffer,
           "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%s\" height=\"%s\""
           " viewBox=\"0 0 %s %s\">\n"
           "  <defs>\n"
           "    <clipPath id=\"mv-viewport\">\n"
           "      <rect width=\"%s\" height=\"%s\"/>\n"
           "    </clipPath>\n"
           "  </defs>\n"
           "  <g clip-path=\"url(#mv-viewport)\">\n",
           width, height, width, height, width, height);
}

static unsigned long ceil_bound(double value)
{
    /* Verified canvas dimensions are positive and at most 2^31 - 1. */
    unsigned long integer = (unsigned long)value;
    return integer + (value != (double)integer);
}

static void eps_begin(mv_buffer *buffer, const mv_scene *scene,
                      const char *width, const char *height)
{
    char hires_width[MV_NUMBER_CAPACITY], hires_height[MV_NUMBER_CAPACITY];

    if (!format_hires_bound(hires_width, width) ||
        !format_hires_bound(hires_height, height)) {
        buffer->status = MV_STATUS_BACKEND_ERROR;
        return;
    }
    append(buffer,
           "%%!PS-Adobe-3.0 EPSF-3.0\n"
           "%%%%BoundingBox: 0 0 %lu %lu\n"
           "%%%%HiResBoundingBox: 0 0 %s %s\n"
           "%%%%LanguageLevel: 2\n"
           "%%%%EndComments\n"
           "gsave\n"
           "0 %s translate\n"
           "1 -1 scale\n"
           "0 0 %s %s rectclip\n"
           "0 setlinecap\n"
           "[] 0 setdash\n",
           ceil_bound(scene->width), ceil_bound(scene->height),
           hires_width, hires_height, height, width, height);
}

static void svg_shape(mv_buffer *buffer, const mv_shape *shape,
                      char n[][MV_NUMBER_CAPACITY])
{
    unsigned r = shape->color[0], g = shape->color[1], b = shape->color[2];

    switch (shape->kind) {
    case MV_RECT:
        append(buffer, "    <rect x=\"%s\" y=\"%s\" width=\"%s\" height=\"%s\""
               " fill=\"#%02x%02x%02x\"/>\n", n[0], n[1], n[2], n[3], r, g, b);
        break;
    case MV_CIRCLE:
        append(buffer, "    <circle cx=\"%s\" cy=\"%s\" r=\"%s\""
               " fill=\"#%02x%02x%02x\"/>\n", n[0], n[1], n[2], r, g, b);
        break;
    case MV_LINE:
        append(buffer, "    <line x1=\"%s\" y1=\"%s\" x2=\"%s\" y2=\"%s\""
               " stroke-width=\"%s\" stroke-linecap=\"butt\" stroke=\"#%02x%02x%02x\"/>\n",
               n[0], n[1], n[2], n[3], n[4], r, g, b);
        break;
    }
}

static void eps_shape(mv_buffer *buffer, const mv_shape *shape,
                      char n[][MV_NUMBER_CAPACITY])
{
    append(buffer, "%u 255 div %u 255 div %u 255 div setrgbcolor\n",
           (unsigned)shape->color[0], (unsigned)shape->color[1], (unsigned)shape->color[2]);

    switch (shape->kind) {
    case MV_RECT:
        append(buffer, "%s %s %s %s rectfill\n", n[0], n[1], n[2], n[3]);
        break;
    case MV_CIRCLE:
        append(buffer, "newpath %s %s %s 0 360 arc closepath fill\n", n[0], n[1], n[2]);
        break;
    case MV_LINE:
        append(buffer, "newpath %s %s moveto %s %s lineto %s setlinewidth stroke\n",
               n[0], n[1], n[2], n[3], n[4]);
        break;
    }
}

mv_status mv_emit(const mv_scene *scene, mv_backend backend, mv_output *output,
                  mv_diagnostic *diag)
{
    static const unsigned value_counts[] = {
        [MV_RECT] = 4, [MV_CIRCLE] = 3, [MV_LINE] = 5
    };
    mv_buffer buffer = {0};
    mv_location loc;
    const char *message = NULL;
    char width[MV_NUMBER_CAPACITY], height[MV_NUMBER_CAPACITY];
    int eps = backend == MV_BACKEND_EPS;
    size_t i;

    if (output)
        *output = (mv_output){0};
    if (!scene || !output || (!eps && backend != MV_BACKEND_SVG))
        return mv_fail(diag, MV_STATUS_INVALID_ARGUMENT, (mv_location){0},
                       "invalid backend emission arguments");

    loc = scene->loc;
    if (eps && (!eps_number_in_range(scene->width) || !eps_number_in_range(scene->height)))
        return mv_fail(diag, MV_STATUS_BACKEND_ERROR, loc,
                       "canvas dimension outside the supported EPS real range");
    if (!format_number(width, scene->width, eps) ||
        !format_number(height, scene->height, eps))
        return mv_fail(diag, MV_STATUS_BACKEND_ERROR, loc, "could not format canvas dimensions");

    if (eps)
        eps_begin(&buffer, scene, width, height);
    else
        svg_begin(&buffer, width, height);

    for (i = 0; i < scene->shape_count && buffer.status == MV_STATUS_OK; ++i) {
        const mv_shape *shape = &scene->shapes[i];
        char numbers[5][MV_NUMBER_CAPACITY];
        unsigned j, count = value_counts[shape->kind];

        loc = shape->loc;
        for (j = 0; j < count; ++j) {
            if (eps && !eps_number_in_range(shape->values[j])) {
                buffer.status = MV_STATUS_BACKEND_ERROR;
                message = "shape number outside the supported EPS real range";
                break;
            }
            if (!format_number(numbers[j], shape->values[j], eps)) {
                buffer.status = MV_STATUS_BACKEND_ERROR;
                message = "could not format a shape number";
                break;
            }
        }
        if (buffer.status != MV_STATUS_OK)
            break;
        if (eps)
            eps_shape(&buffer, shape, numbers);
        else
            svg_shape(&buffer, shape, numbers);
    }
    if (buffer.status == MV_STATUS_OK) {
        loc = scene->loc;
        append(&buffer, "%s", eps ? "grestore\n%%EOF\n" : "  </g>\n</svg>\n");
    }

    if (buffer.status == MV_STATUS_OK) {
        *output = buffer.output;
        return MV_STATUS_OK;
    }

    free(buffer.output.data);
    if (buffer.status == MV_STATUS_OUT_OF_MEMORY)
        message = "out of memory building backend output";
    else if (buffer.status == MV_STATUS_LIMIT_EXCEEDED)
        message = "backend output size limit exceeded";
    else if (!message)
        message = "could not format backend output";
    return mv_fail(diag, buffer.status, loc, message);
}
