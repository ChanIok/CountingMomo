#include "utils/image.hpp"

#include "vendor/std.hpp"

#include "vendor/windows.hpp"
#include "vendor/windows/ocidl.hpp"
#include "vendor/windows/wincodec.hpp"
#include "vendor/windows/wrl/client.hpp"

#include "utils/string.hpp"

namespace utils::image {
auto load(const std::filesystem::path& path) -> std::expected<cv::Mat, std::string> {
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  ComPtr<IWICBitmapDecoder> decoder;
  if (SUCCEEDED(hr))
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &decoder);
  ComPtr<IWICBitmapFrameDecode> frame;
  if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
  ComPtr<IWICFormatConverter> converter;
  if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr))
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                               nullptr, 0, WICBitmapPaletteTypeCustom);
  UINT width{}, height{};
  if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
  if (SUCCEEDED(hr) &&
      (!width || !height || width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
       height > static_cast<UINT>(std::numeric_limits<int>::max()) ||
       static_cast<std::uint64_t>(width) * height * 3 > std::numeric_limits<UINT>::max()))
    hr = E_INVALIDARG;
  cv::Mat pixels;
  if (SUCCEEDED(hr)) {
    pixels.create(static_cast<int>(height), static_cast<int>(width), CV_8UC3);
    UINT stride = static_cast<UINT>(pixels.step[0]);
    hr = converter->CopyPixels(nullptr, stride, stride * height, pixels.data);
  }
  if (FAILED(hr))
    return std::unexpected(std::format("读取页面图片失败（0x{:08x}）：{}",
                                       static_cast<std::uint32_t>(hr), string::path_utf8(path)));
  return pixels;
}

namespace {
auto save(const std::filesystem::path& path, const cv::Mat& bgr, bool jpeg)
    -> std::expected<void, std::string> {
  if (bgr.empty() || bgr.type() != CV_8UC3) return std::unexpected("保存图片需要BGR三通道像素。");
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  ComPtr<IWICStream> stream;
  if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  ComPtr<IWICBitmapEncoder> encoder;
  if (SUCCEEDED(hr))
    hr = factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr,
                                &encoder);
  if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (SUCCEEDED(hr))
    hr = encoder->CreateNewFrame(&frame, jpeg ? properties.GetAddressOf() : nullptr);
  if (SUCCEEDED(hr) && jpeg) {
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value{};
    value.vt = VT_R4;
    value.fltVal = 1.0f;
    hr = properties->Write(1, &option, &value);
  }
  if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
  if (SUCCEEDED(hr)) hr = frame->SetSize(bgr.cols, bgr.rows);
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
  if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) hr = E_UNEXPECTED;
  auto pixels = bgr.isContinuous() ? bgr : bgr.clone();
  UINT stride = static_cast<UINT>(pixels.step[0]);
  if (SUCCEEDED(hr))
    hr = frame->WritePixels(pixels.rows, stride, stride * pixels.rows, pixels.data);
  if (SUCCEEDED(hr)) hr = frame->Commit();
  if (SUCCEEDED(hr)) hr = encoder->Commit();
  if (FAILED(hr))
    return std::unexpected(std::format("保存图片失败（0x{:08x}）：{}",
                                       static_cast<std::uint32_t>(hr), string::path_utf8(path)));
  return {};
}
}  // namespace

auto save_png(const std::filesystem::path& path, const cv::Mat& bgr)
    -> std::expected<void, std::string> {
  return save(path, bgr, false);
}

auto save_jpg(const std::filesystem::path& path, const cv::Mat& bgr)
    -> std::expected<void, std::string> {
  return save(path, bgr, true);
}
}  // namespace utils::image
