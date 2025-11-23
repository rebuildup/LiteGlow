#define NOMINMAX
#include "LiteGlow.h"

#include <vector>
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>
#include <limits>

// -----------------------------------------------------------------------------
// Constants & Helpers
// -----------------------------------------------------------------------------

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

template <typename T>
static inline T Clamp(T val, T minVal, T maxVal) {
    return std::max(minVal, std::min(val, maxVal));
}

// -----------------------------------------------------------------------------
// Pixel Traits
// -----------------------------------------------------------------------------

template <typename PixelT>
struct LiteGlowPixelTraits;

template <>
struct LiteGlowPixelTraits<PF_Pixel> {
    using ChannelType = A_u_char;
    static constexpr float MAX_VAL = 255.0f;
    static inline float ToFloat(ChannelType v) { return static_cast<float>(v); }
    static inline ChannelType FromFloat(float v) { return static_cast<ChannelType>(Clamp(v, 0.0f, MAX_VAL) + 0.5f); }
};

template <>
struct LiteGlowPixelTraits<PF_Pixel16> {
    using ChannelType = A_u_short;
    static constexpr float MAX_VAL = 32768.0f;
    static inline float ToFloat(ChannelType v) { return static_cast<float>(v); }
    static inline ChannelType FromFloat(float v) { return static_cast<ChannelType>(Clamp(v, 0.0f, MAX_VAL) + 0.5f); }
};

template <>
struct LiteGlowPixelTraits<PF_PixelFloat> {
    using ChannelType = PF_FpShort;
    static constexpr float MAX_VAL = 1.0f;
    static inline float ToFloat(ChannelType v) { return static_cast<float>(v); }
    static inline ChannelType FromFloat(float v) { return static_cast<ChannelType>(v); }
};

// -----------------------------------------------------------------------------
// IIR Gaussian Blur
// -----------------------------------------------------------------------------

// Simple IIR Gaussian Blur approximation (Young-van Vliet or similar)
// Based on "Recursive Gaussian derivative filters" (Van Vliet et al.)
// or a simplified version.
// Using a 3rd order IIR filter for good quality.

struct IIRCoeffs {
    float n0, n1, n2, d1, d2, d3;
};

static IIRCoeffs CalcIIRCoeffs(float sigma) {
    // Coefficients from "Recursive Gaussian derivative filters"
    // q = sigma (if q < 2.5, use q = 2.5 - 0.15*(2.5-q) approx? No, standard formula)
    // Actually, for sigma < 0.5, IIR is unstable/inaccurate.
    if (sigma < 0.5f) sigma = 0.5f;
    
    float q = sigma;
    if (q > 2.5f) {
        q = 0.98711f * sigma - 0.96330f;
    } else {
        q = 3.97156f - 4.14554f * std::sqrt(1.0f - 0.26891f * sigma);
    }
    
    float b0 = 1.57825f + 2.44413f * q + 1.4281f * q * q + 0.422205f * q * q * q;
    float b1 = 2.44413f * q + 2.85619f * q * q + 1.26661f * q * q * q;
    float b2 = -1.4281f * q * q - 1.26661f * q * q * q;
    float b3 = 0.422205f * q * q * q;
    
    float B = 1.0f - (b1 + b2 + b3) / b0;
    
    IIRCoeffs c;
    c.n0 = B; // For 0th order (smoothing)
    c.n1 = 0.0f; // Not used for smoothing
    c.n2 = 0.0f;
    c.d1 = b1 / b0;
    c.d2 = b2 / b0;
    c.d3 = b3 / b0;
    
    return c;
}

// Apply 1D IIR filter (Forward and Backward)
// data: input/output array (stride 1)
// count: number of elements
static void IIR_1D(float* data, int count, const IIRCoeffs& c) {
    if (count <= 0) return;

    // Forward
    // y[n] = n0*x[n] + (n1*x[n-1] + n2*x[n-2]) - (d1*y[n-1] + d2*y[n-2] + d3*y[n-3])
    // Simplified for smoothing (n1=n2=0):
    // y[n] = n0*x[n] - d1*y[n-1] - d2*y[n-2] - d3*y[n-3]
    
    // Boundary condition: extend edge
    float val0 = data[0];
    float y1 = val0, y2 = val0, y3 = val0;
    
    for (int i = 0; i < count; ++i) {
        float x = data[i];
        float y = c.n0 * x - (c.d1 * y1 + c.d2 * y2 + c.d3 * y3);
        data[i] = y;
        y3 = y2; y2 = y1; y1 = y;
    }
    
    // Backward
    // y[n] = n0*x[n] - d1*y[n+1] - d2*y[n+2] - d3*y[n+3]
    // Note: For symmetric Gaussian, we apply the same filter backward.
    // But usually coefficients are slightly different or we normalize?
    // Van Vliet: Apply same causal filter forward and backward.
    
    float valN = data[count - 1];
    y1 = valN; y2 = valN; y3 = valN;
    
    for (int i = count - 1; i >= 0; --i) {
        float x = data[i];
        float y = c.n0 * x - (c.d1 * y1 + c.d2 * y2 + c.d3 * y3);
        data[i] = y;
        y3 = y2; y2 = y1; y1 = y;
    }
}

    const A_u_char* input_base = reinterpret_cast<const A_u_char*>(input->data);
    A_u_char* output_base = reinterpret_cast<A_u_char*>(output->data);
    const A_long input_rowbytes = input->rowbytes;
    const A_long output_rowbytes = output->rowbytes;

    // Parameters
    float strength = static_cast<float>(params[LITEGLOW_STRENGTH]->u.fs_d.value);
    float radius = static_cast<float>(params[LITEGLOW_RADIUS]->u.sd.value);
    float threshold = static_cast<float>(params[LITEGLOW_THRESHOLD]->u.sd.value) * (LiteGlowPixelTraits<Pixel>::MAX_VAL / 255.0f);
    int quality = params[LITEGLOW_QUALITY]->u.pd.value;

    if (radius < 0.1f) {
        return PF_COPY(input, output, NULL, NULL);
    }

    // Allocate float buffer for processing (R, G, B, A)
    // Interleaved: R, G, B, A
    std::vector<float> buffer(width * height * 4);

    // 1. Threshold and Copy to Buffer
    int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;

    auto threshold_pass = [&](int start_y, int end_y) {
        for (int y = start_y; y < end_y; ++y) {
            const Pixel* row = reinterpret_cast<const Pixel*>(input_base + y * input_rowbytes);
            float* buf_row = &buffer[y * width * 4];
            
            for (int x = 0; x < width; ++x) {
                float r = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].red);
                float g = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].green);
                float b = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].blue);
                float a = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].alpha);
                
                // Luma for threshold
                float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                
                if (luma > threshold) {
                    float factor = (luma - threshold) / (LiteGlowPixelTraits<Pixel>::MAX_VAL - threshold + 0.001f); // Normalize 0..1
                    float alpha = std::min(1.0f, (luma - threshold) / (LiteGlowPixelTraits<Pixel>::MAX_VAL * 0.1f)); // Soft knee
                    
                    buf_row[x * 4 + 0] = r * alpha;
                    buf_row[x * 4 + 1] = g * alpha;
                    buf_row[x * 4 + 2] = b * alpha;
                    buf_row[x * 4 + 3] = a;
                } else {
                    buf_row[x * 4 + 0] = 0.0f;
                    buf_row[x * 4 + 1] = 0.0f;
                    buf_row[x * 4 + 2] = 0.0f;
                    buf_row[x * 4 + 3] = 0.0f;
                }
            }
        }
    };

    int rows_per_thread = (height + num_threads - 1) / num_threads;
    for (int i = 0; i < num_threads; ++i) {
        int start = i * rows_per_thread;
        int end = std::min(start + rows_per_thread, height);
        if (start < end) threads.emplace_back(threshold_pass, start, end);
    }
    for (auto& t : threads) t.join();
    threads.clear();

    // 2. IIR Blur
    IIRCoeffs coeffs = CalcIIRCoeffs(radius);

    // Horizontal Pass
    auto blur_h = [&](int start_y, int end_y) {
        std::vector<float> line_r(width);
        std::vector<float> line_g(width);
        std::vector<float> line_b(width);
        
        for (int y = start_y; y < end_y; ++y) {
            float* buf_row = &buffer[y * width * 4];
            
            // De-interleave
            for (int x = 0; x < width; ++x) {
                line_r[x] = buf_row[x * 4 + 0];
                line_g[x] = buf_row[x * 4 + 1];
                line_b[x] = buf_row[x * 4 + 2];
            }
            
            IIR_1D(line_r.data(), width, coeffs);
            IIR_1D(line_g.data(), width, coeffs);
            IIR_1D(line_b.data(), width, coeffs);
            
            // Interleave
            for (int x = 0; x < width; ++x) {
                buf_row[x * 4 + 0] = line_r[x];
                buf_row[x * 4 + 1] = line_g[x];
                buf_row[x * 4 + 2] = line_b[x];
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        int start = i * rows_per_thread;
        int end = std::min(start + rows_per_thread, height);
        if (start < end) threads.emplace_back(blur_h, start, end);
    }
    for (auto& t : threads) t.join();
    threads.clear();

    // Vertical Pass
    int cols_per_thread = (width + num_threads - 1) / num_threads;
    auto blur_v = [&](int start_x, int end_x) {
        std::vector<float> col_r(height);
        std::vector<float> col_g(height);
        std::vector<float> col_b(height);
        
        for (int x = start_x; x < end_x; ++x) {
            // De-interleave
            for (int y = 0; y < height; ++y) {
                float* buf_row = &buffer[y * width * 4];
                col_r[y] = buf_row[x * 4 + 0];
                col_g[y] = buf_row[x * 4 + 1];
                col_b[y] = buf_row[x * 4 + 2];
            }
            
            IIR_1D(col_r.data(), height, coeffs);
            IIR_1D(col_g.data(), height, coeffs);
            IIR_1D(col_b.data(), height, coeffs);
            
            // Interleave
            for (int y = 0; y < height; ++y) {
                float* buf_row = &buffer[y * width * 4];
                buf_row[x * 4 + 0] = col_r[y];
                buf_row[x * 4 + 1] = col_g[y];
                buf_row[x * 4 + 2] = col_b[y];
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        int start = i * cols_per_thread;
        int end = std::min(start + cols_per_thread, width);
        if (start < end) threads.emplace_back(blur_v, start, end);
    }
    for (auto& t : threads) t.join();
    threads.clear();

    // 3. Composite
    // Additive blend: Output = Input + Glow * Strength
    float strength_norm = strength / 100.0f; 

    auto composite_pass = [&](int start_y, int end_y) {
        for (int y = start_y; y < end_y; ++y) {
            Pixel* out_row = reinterpret_cast<Pixel*>(output_base + y * output_rowbytes);
            const Pixel* in_row = reinterpret_cast<const Pixel*>(input_base + y * input_rowbytes);
            float* buf_row = &buffer[y * width * 4];
            
            for (int x = 0; x < width; ++x) {
                float gr = buf_row[x * 4 + 0] * strength_norm;
                float gg = buf_row[x * 4 + 1] * strength_norm;
                float gb = buf_row[x * 4 + 2] * strength_norm;
                
                float ir = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].red);
                float ig = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].green);
                float ib = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].blue);
                float ia = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].alpha);
                
                // Additive
                out_row[x].red = LiteGlowPixelTraits<Pixel>::FromFloat(ir + gr);
                out_row[x].green = LiteGlowPixelTraits<Pixel>::FromFloat(ig + gg);
                out_row[x].blue = LiteGlowPixelTraits<Pixel>::FromFloat(ib + gb);
                out_row[x].alpha = in_row[x].alpha; 
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        int start = i * rows_per_thread;
        int end = std::min(start + rows_per_thread, height);
        if (start < end) threads.emplace_back(composite_pass, start, end);
    }
    for (auto& t : threads) t.join();

    return PF_Err_NONE;
}

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    int bpp = (output->width > 0) ? (output->rowbytes / output->width) : 0;
    if (bpp == sizeof(PF_PixelFloat)) {
        return RenderGeneric<PF_PixelFloat>(in_data, out_data, params, output);
    } else if (bpp == sizeof(PF_Pixel16)) {
        return RenderGeneric<PF_Pixel16>(in_data, out_data, params, output);
    } else {
        return RenderGeneric<PF_Pixel>(in_data, out_data, params, output);
    }
}

static PF_Err
About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
```
    // Parameters
    float strength = static_cast<float>(params[LITEGLOW_STRENGTH]->u.fs_d.value);
    float radius = static_cast<float>(params[LITEGLOW_RADIUS]->u.sd.value);
    float threshold = static_cast<float>(params[LITEGLOW_THRESHOLD]->u.sd.value) * (LiteGlowPixelTraits<Pixel>::MAX_VAL / 255.0f);
    int quality = params[LITEGLOW_QUALITY]->u.pd.value;

    if (radius < 0.1f) {
        return PF_COPY(input, output, NULL, NULL);
    }

    // Allocate float buffer for processing (R, G, B, A)
    // Interleaved: R, G, B, A
    std::vector<float> buffer(width * height * 4);

    // 1. Threshold and Copy to Buffer
    int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;

    auto threshold_pass = [&](int start_y, int end_y) {
        for (int y = start_y; y < end_y; ++y) {
            const Pixel* row = reinterpret_cast<const Pixel*>(input_base + y * input_rowbytes);
            float* buf_row = &buffer[y * width * 4];
            
            for (int x = 0; x < width; ++x) {
                float r = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].red);
                float g = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].green);
                float b = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].blue);
                float a = LiteGlowPixelTraits<Pixel>::ToFloat(row[x].alpha);
                
                // Luma for threshold
                float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                
                if (luma > threshold) {
                    float factor = (luma - threshold) / (LiteGlowPixelTraits<Pixel>::MAX_VAL - threshold + 0.001f); // Normalize 0..1
                    float alpha = std::min(1.0f, (luma - threshold) / (LiteGlowPixelTraits<Pixel>::MAX_VAL * 0.1f)); // Soft knee
                    
                    buf_row[x * 4 + 0] = r * alpha;
                    buf_row[x * 4 + 1] = g * alpha;
                    buf_row[x * 4 + 2] = b * alpha;
                    buf_row[x * 4 + 3] = a;
                } else {
                    buf_row[x * 4 + 0] = 0.0f;
                    buf_row[x * 4 + 1] = 0.0f;
                    buf_row[x * 4 + 2] = 0.0f;
                    buf_row[x * 4 + 3] = 0.0f;
                }
            }
        }
    };

    int rows_per_thread = (height + num_threads - 1) / num_threads;
    for (int i = 0; i < num_threads; ++i) {
        int start = i * rows_per_thread;
        int end = std::min(start + rows_per_thread, height);
        if (start < end) threads.emplace_back(threshold_pass, start, end);
    }
    for (auto& t : threads) t.join();
    threads.clear();

    // 2. IIR Blur
    IIRCoeffs coeffs = CalcIIRCoeffs(radius);

    // Horizontal Pass
    auto blur_h = [&](int start_y, int end_y) {
        std::vector<float> line_r(width);
        std::vector<float> line_g(width);
        std::vector<float> line_b(width);
        
        for (int y = start_y; y < end_y; ++y) {
            float* buf_row = &buffer[y * width * 4];
            
            // De-interleave
            for (int x = 0; x < width; ++x) {
                line_r[x] = buf_row[x * 4 + 0];
                line_g[x] = buf_row[x * 4 + 1];
                line_b[x] = buf_row[x * 4 + 2];
            }
            
            IIR_1D(line_r.data(), width, coeffs);
            IIR_1D(line_g.data(), width, coeffs);
            IIR_1D(line_b.data(), width, coeffs);
            
            // Interleave
            for (int x = 0; x < width; ++x) {
                buf_row[x * 4 + 0] = line_r[x];
                buf_row[x * 4 + 1] = line_g[x];
                buf_row[x * 4 + 2] = line_b[x];
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        int start = i * rows_per_thread;
        int end = std::min(start + rows_per_thread, height);
        if (start < end) threads.emplace_back(blur_h, start, end);
    }
    for (auto& t : threads) t.join();
    threads.clear();

    // Vertical Pass
    int cols_per_thread = (width + num_threads - 1) / num_threads;
    auto blur_v = [&](int start_x, int end_x) {
        std::vector<float> col_r(height);
        std::vector<float> col_g(height);
        std::vector<float> col_b(height);
        
        for (int x = start_x; x < end_x; ++x) {
            // De-interleave
            for (int y = 0; y < height; ++y) {
                float* buf_row = &buffer[y * width * 4];
                col_r[y] = buf_row[x * 4 + 0];
                col_g[y] = buf_row[x * 4 + 1];
                col_b[y] = buf_row[x * 4 + 2];
            }
            
            IIR_1D(col_r.data(), height, coeffs);
            IIR_1D(col_g.data(), height, coeffs);
            IIR_1D(col_b.data(), height, coeffs);
            
            // Interleave
            for (int y = 0; y < height; ++y) {
                float* buf_row = &buffer[y * width * 4];
                buf_row[x * 4 + 0] = col_r[y];
                buf_row[x * 4 + 1] = col_g[y];
                buf_row[x * 4 + 2] = col_b[y];
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        int start = i * cols_per_thread;
        int end = std::min(start + cols_per_thread, width);
        if (start < end) threads.emplace_back(blur_v, start, end);
    }
    for (auto& t : threads) t.join();
    threads.clear();

    // 3. Composite
    // Additive blend: Output = Input + Glow * Strength
    float strength_norm = strength / 100.0f; 

    auto composite_pass = [&](int start_y, int end_y) {
        for (int y = start_y; y < end_y; ++y) {
            Pixel* out_row = reinterpret_cast<Pixel*>(output_base + y * output_rowbytes);
            const Pixel* in_row = reinterpret_cast<const Pixel*>(input_base + y * input_rowbytes);
            float* buf_row = &buffer[y * width * 4];
            
            for (int x = 0; x < width; ++x) {
                float gr = buf_row[x * 4 + 0] * strength_norm;
                float gg = buf_row[x * 4 + 1] * strength_norm;
                float gb = buf_row[x * 4 + 2] * strength_norm;
                
                float ir = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].red);
                float ig = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].green);
                float ib = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].blue);
                float ia = LiteGlowPixelTraits<Pixel>::ToFloat(in_row[x].alpha);
                
                // Additive
                out_row[x].red = LiteGlowPixelTraits<Pixel>::FromFloat(ir + gr);
                out_row[x].green = LiteGlowPixelTraits<Pixel>::FromFloat(ig + gg);
                out_row[x].blue = LiteGlowPixelTraits<Pixel>::FromFloat(ib + gb);
                out_row[x].alpha = in_row[x].alpha; 
            }
        }
    };

    for (int i = 0; i < num_threads; ++i) {
        int start = i * rows_per_thread;
        int end = std::min(start + rows_per_thread, height);
        if (start < end) threads.emplace_back(composite_pass, start, end);
    }
    for (auto& t : threads) t.join();

    return PF_Err_NONE;
}

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
    int bpp = (output->width > 0) ? (output->rowbytes / output->width) : 0;
    if (bpp == sizeof(PF_PixelFloat)) {
        return RenderGeneric<PF_PixelFloat>(in_data, out_data, params, output);
    } else if (bpp == sizeof(PF_Pixel16)) {
        return RenderGeneric<PF_Pixel16>(in_data, out_data, params, output);
    } else {
        return RenderGeneric<PF_Pixel>(in_data, out_data, params, output);
    }
}

static PF_Err
About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        STR(StrID_Description));
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);

    PF_ADD_FLOAT_SLIDERX(
        "Strength",
        STRENGTH_MIN,
        STRENGTH_MAX,
        STRENGTH_MIN,
        STRENGTH_MAX,
        STRENGTH_DFLT,
        PF_Precision_TENTHS,
        0,
        0,
        LITEGLOW_STRENGTH);

    PF_ADD_FLOAT_SLIDERX(
        "Radius",
        RADIUS_MIN,
        RADIUS_MAX,
        RADIUS_MIN,
        RADIUS_MAX,
        RADIUS_DFLT,
        PF_Precision_TENTHS,
        0,
        0,
        LITEGLOW_RADIUS);

    PF_ADD_FLOAT_SLIDERX(
        "Threshold",
        THRESHOLD_MIN,
        THRESHOLD_MAX,
        THRESHOLD_MIN,
        THRESHOLD_MAX,
        THRESHOLD_DFLT,
        PF_Precision_INTEGER,
        0,
        0,
        LITEGLOW_THRESHOLD);

    PF_ADD_POPUP(
        "Quality",
        QUALITY_NUM_CHOICES,
        QUALITY_DFLT,
        "Low|Medium|High",
        LITEGLOW_QUALITY);

    out_data->num_params = LITEGLOW_NUM_PARAMS;
    return PF_Err_NONE;
}

extern "C" DllExport
PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr,
    PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite * inSPBasicSuitePtr,
    const char* inHostName,
    const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(
        inPtr,
        inPluginDataCallBackPtr,
        "LiteGlow", // Name
        "361do LiteGlow", // Match Name
        "361do_plugins", // Category
        AE_RESERVED_INFO,
        "EffectMain",
        "https://github.com/rebuildup/LiteGlow");
    return result;
}

extern "C" DllExport
PF_Err EffectMain(PF_Cmd cmd,
    PF_InData * in_data,
    PF_OutData * out_data,
    PF_ParamDef * params[],
    PF_LayerDef * output,
    void* extra)
{
    PF_Err err = PF_Err_NONE;
    try {
    return err;
}