#include "utils/image.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/wincodec.hpp"
#include "vendor/windows/wrl/client.hpp"

#include "core/message/message.hpp"
#include "utils/string.hpp"

namespace utils::image {

// 使用 WIC 解码图像文件并转换为 24 位 BGR cv::Mat 矩阵
auto load(const std::filesystem::path& path) -> std::expected<cv::Mat, core::Error> {
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  // 创建 Windows Imaging Component (WIC) 工厂
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  ComPtr<IWICBitmapDecoder> decoder;
  // 从目标文件路径创建图像解码器
  if (SUCCEEDED(hr)) {
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  // 提取图像第一帧
  if (SUCCEEDED(hr)) {
    hr = decoder->GetFrame(0, &frame);
  }
  ComPtr<IWICFormatConverter> converter;
  // 创建格式转换器并初始化为 24 位 BGR 格式
  if (SUCCEEDED(hr)) {
    hr = factory->CreateFormatConverter(&converter);
  }
  if (SUCCEEDED(hr)) {
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                               nullptr, 0, WICBitmapPaletteTypeCustom);
  }
  UINT width{};
  UINT height{};
  if (SUCCEEDED(hr)) {
    hr = converter->GetSize(&width, &height);
  }
  // 校验尺寸合法性与溢出风险
  if (SUCCEEDED(hr) &&
      (!width || !height || width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
       height > static_cast<UINT>(std::numeric_limits<int>::max()) ||
       static_cast<std::uint64_t>(width) * height * 3 > std::numeric_limits<UINT>::max())) {
    hr = E_INVALIDARG;
  }
  cv::Mat pixels;
  // 分配 OpenCV 矩阵并将转换后的像素数据拷贝拷出
  if (SUCCEEDED(hr)) {
    pixels.create(static_cast<int>(height), static_cast<int>(width), CV_8UC3);
    UINT stride = static_cast<UINT>(pixels.step[0]);
    hr = converter->CopyPixels(nullptr, stride, stride * height, pixels.data);
  }
  if (FAILED(hr)) {
    return std::unexpected(core::error("error.image.read_failed",
                                       std::format("{:08x}", static_cast<std::uint32_t>(hr)),
                                       string::path_utf8(path)));
  }
  return pixels;
}

// 使用 WIC 将 24 位 BGR 像素矩阵编码保存为本地 PNG 文件
auto save_png(const std::filesystem::path& path, const cv::Mat& bgr)
    -> std::expected<void, core::Error> {
  // 必须为非空的 3 通道 8 位 BGR 矩阵
  if (bgr.empty() || bgr.type() != CV_8UC3) {
    return std::unexpected(core::error("error.image.invalid_pixels"));
  }
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  // 创建 WIC 工厂与目标文件输出流
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  ComPtr<IWICStream> stream;
  if (SUCCEEDED(hr)) {
    hr = factory->CreateStream(&stream);
  }
  if (SUCCEEDED(hr)) {
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  }
  ComPtr<IWICBitmapEncoder> encoder;
  // 创建并初始化 PNG 容器编码器
  if (SUCCEEDED(hr)) {
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  // 创建新帧并配置尺寸与像素格式
  if (SUCCEEDED(hr)) {
    hr = encoder->CreateNewFrame(&frame, nullptr);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Initialize(nullptr);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->SetSize(bgr.cols, bgr.rows);
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) {
    hr = frame->SetPixelFormat(&format);
  }
  if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
    hr = E_UNEXPECTED;
  }
  // 确保内存连续后写入帧像素数据
  auto pixels = bgr.isContinuous() ? bgr : bgr.clone();
  UINT stride = static_cast<UINT>(pixels.step[0]);
  if (SUCCEEDED(hr)) {
    hr = frame->WritePixels(pixels.rows, stride, stride * pixels.rows, pixels.data);
  }
  // 提交帧并持久化编码结果
  if (SUCCEEDED(hr)) {
    hr = frame->Commit();
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Commit();
  }
  if (FAILED(hr)) {
    return std::unexpected(core::error("error.image.save_failed",
                                       std::format("{:08x}", static_cast<std::uint32_t>(hr)),
                                       string::path_utf8(path)));
  }
  return {};
}

}  // namespace utils::image
