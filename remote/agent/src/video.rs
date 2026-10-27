// Copyright 2026 The Umbra Authors.
//! The phone's picture as H.264 video, encoded by the graphics card.
//!
//! Windows Graphics Capture takes Umbra's (off-screen) window as Chrome
//! composites it; each new image is cropped to the page, converted to NV12
//! by the GPU's video processor and handed to the card's own H.264 encoder
//! (NVENC, AMF or Quick Sync, through Media Foundation). Nothing leaves the
//! GPU until the encoded frame does. Compared with DevTools' JPEG
//! screencast that is a frame per vsync instead of per round trip, a tenth
//! of the bytes, and the page at the phone's own pixel density.
//!
//! Output: Annex B access units, each prefixed with the protocol's kind
//! byte (3), ready to go to the phone as one binary message.

use std::mem::ManuallyDrop;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use windows::Graphics::Capture::{Direct3D11CaptureFrame, Direct3D11CaptureFramePool, GraphicsCaptureItem, GraphicsCaptureSession};
use windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
use windows::Graphics::DirectX::DirectXPixelFormat;
use windows::Graphics::SizeInt32;
use windows::Win32::Foundation::{HMODULE, HWND};
use windows::Win32::Graphics::Direct3D::D3D_DRIVER_TYPE_HARDWARE;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::Win32::Graphics::Dxgi::IDXGIDevice;
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::CoTaskMemFree;
use windows::Win32::System::Variant::VARIANT;
use windows::Win32::System::WinRT::Direct3D11::{CreateDirect3D11DeviceFromDXGIDevice, IDirect3DDxgiInterfaceAccess};
use windows::Win32::System::WinRT::Graphics::Capture::IGraphicsCaptureItemInterop;
use windows::Win32::System::WinRT::{RO_INIT_MULTITHREADED, RoInitialize};
use windows::core::{Interface, Result};

use crate::log;
use crate::window;

/// Encoded frames that may be on their way to the phone (sent, not yet
/// acked). Past this, new pictures wait on the GPU instead of piling up in
/// the Wi-Fi queue, so a stall costs a frame or two and not seconds.
pub const FRAMES_IN_FLIGHT: u32 = 4;

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Settings {
  /// Picture size in physical pixels (even).
  pub width: u32,
  pub height: u32,
  pub bitrate: u32,
  pub fps: u32,
  /// Where the window belongs (x, y, w, h). Chrome moves it when a page
  /// goes in or out of full screen; it is put straight back, and the crop
  /// follows the page within it.
  pub place: (i32, i32, i32, i32),
}

pub struct Video {
  pub settings: Settings,
  pub hwnd: isize,
  stop: Arc<AtomicBool>,
  key: Arc<AtomicBool>,
  /// Bits per second the network can take right now (0: settings.bitrate).
  rate: Arc<AtomicU32>,
  thread: Option<JoinHandle<()>>,
}

impl Video {
  /// Makes the next frame a keyframe (a new or confused decoder can start
  /// from it).
  pub fn request_key(&self) {
    self.key.store(true, Ordering::Relaxed);
  }

  /// Follows the network: encode at up to `bps`, never above the phone's
  /// quality preset (settings.bitrate) and not below 1 Mbit/s.
  pub fn set_bitrate(&self, bps: u32) {
    self.rate.store(bps.clamp(1_000_000, self.settings.bitrate.max(1_000_000)), Ordering::Relaxed);
  }
}

impl Drop for Video {
  fn drop(&mut self) {
    self.stop.store(true, Ordering::Relaxed);
    if let Some(t) = self.thread.take() {
      let _ = t.join();
    }
  }
}

/// Starts capturing and encoding `hwnd`. Fails (and the caller falls back
/// to the JPEG screencast) when there is no hardware encoder or the window
/// can't be captured.
pub fn start(
  hwnd: isize,
  settings: Settings,
  out: impl Fn(Vec<u8>) + Send + 'static,
  in_flight: Arc<AtomicU32>,
) -> std::result::Result<Video, String> {
  let stop = Arc::new(AtomicBool::new(false));
  let key = Arc::new(AtomicBool::new(false));
  let rate = Arc::new(AtomicU32::new(0));
  let (ready_tx, ready_rx) = std::sync::mpsc::channel();
  let thread = {
    let (stop, key, rate) = (stop.clone(), key.clone(), rate.clone());
    std::thread::Builder::new()
      .name("video".into())
      .spawn(move || {
        let mut pipe = match Pipeline::new(hwnd, settings) {
          Ok(p) => {
            let _ = ready_tx.send(Ok(p.encoder.name.clone()));
            p
          }
          Err(e) => {
            let _ = ready_tx.send(Err(format!("{e}")));
            return;
          }
        };
        if let Err(e) = pipe.run(&stop, &key, &rate, &out, &in_flight) {
          log!("video stopped: {e}");
        }
      })
      .map_err(|e| e.to_string())?
  };
  match ready_rx.recv_timeout(Duration::from_secs(10)) {
    Ok(Ok(name)) => {
      log!("video: {}x{} at {} kbit/s on {name}", settings.width, settings.height, settings.bitrate / 1000);
      Ok(Video { settings, hwnd, stop, key, rate, thread: Some(thread) })
    }
    Ok(Err(e)) => Err(e),
    Err(_) => {
      stop.store(true, Ordering::Relaxed);
      Err("video setup timed out".into())
    }
  }
}

struct Pipeline {
  ctx: ID3D11DeviceContext,
  capture: Capture,
  convert: Converter,
  encoder: Encoder,
  hwnd: isize,
  settings: Settings,
}

impl Pipeline {
  fn new(hwnd: isize, settings: Settings) -> Result<Pipeline> {
    unsafe {
      let _ = RoInitialize(RO_INIT_MULTITHREADED);
      MFStartup(MF_VERSION, MFSTARTUP_LITE)?;
    }
    let (dev, ctx) = device()?;
    let encoder = Encoder::new(&dev, settings)?;
    let capture = Capture::new(&dev, hwnd, settings.fps)?;
    let convert = Converter::new(&dev, &ctx, settings.width, settings.height)?;
    Ok(Pipeline { ctx, capture, convert, encoder, hwnd, settings })
  }

  fn run(
    &mut self,
    stop: &AtomicBool,
    key: &AtomicBool,
    rate: &AtomicU32,
    out: &dyn Fn(Vec<u8>),
    in_flight: &AtomicU32,
  ) -> Result<()> {
    let start = Instant::now();
    let mut bitrate = self.settings.bitrate;
    // Frames lost on the way are acked by the phone (umbranet reports them),
    // so this is only a safety net: never wait on missing acks forever.
    let mut full_since: Option<Instant> = None;
    let frame_time = Duration::from_secs(1) / self.settings.fps;
    let mut origin = (0, 0);
    let mut origin_at: Option<Instant> = None;
    // The newest converted picture not yet encoded, and the last one that was.
    let mut fresh: Option<ID3D11Texture2D> = None;
    let mut last: Option<ID3D11Texture2D> = None;
    let mut last_fed = Instant::now();
    // After the page stops changing, the last picture is encoded a few more
    // times: each pass sharpens what a fast scroll had to blur, and it
    // flushes anything the encoder held back.
    let mut repeats = 0u32;
    let mut placed_at: Option<Instant> = None;
    while !stop.load(Ordering::Relaxed) {
      let p = self.settings.place;
      if window::rect(self.hwnd) != Some(p) && placed_at.is_none_or(|t| t.elapsed() > Duration::from_millis(20)) {
        window::place(self.hwnd, p.0, p.1, p.2, p.3);
        placed_at = Some(Instant::now());
        origin_at = None;
      }
      self.encoder.poll(out, in_flight)?;
      if origin_at.is_none_or(|t| t.elapsed() > Duration::from_millis(250)) {
        // The page moves inside the window when an infobar comes or goes.
        if let Some(o) = window::page_origin(self.hwnd) {
          origin = o;
        }
        origin_at = Some(Instant::now());
      }
      let mut newest = None;
      while let Some(f) = self.capture.next()? {
        newest = Some(f);
      }
      if let Some(frame) = newest {
        let tex: ID3D11Texture2D = unsafe { frame.Surface()?.cast::<IDirect3DDxgiInterfaceAccess>()?.GetInterface()? };
        fresh = Some(self.convert.run(&self.ctx, &tex, origin, self.capture.size)?);
        let _ = frame.Close();
      }
      let want = match rate.load(Ordering::Relaxed) {
        0 => self.settings.bitrate,
        r => r,
      };
      if want.abs_diff(bitrate) > bitrate / 20 {
        self.encoder.set_bitrate(want);
        bitrate = want;
      }
      if in_flight.load(Ordering::Relaxed) >= FRAMES_IN_FLIGHT {
        let since = *full_since.get_or_insert_with(Instant::now);
        if since.elapsed() > Duration::from_secs(1) {
          in_flight.store(0, Ordering::Relaxed);
          key.store(true, Ordering::Relaxed);
          full_since = None;
        }
      } else {
        full_since = None;
      }
      let want_key = key.load(Ordering::Relaxed);
      if self.encoder.need > 0 && in_flight.load(Ordering::Relaxed) < FRAMES_IN_FLIGHT {
        let pick = if let Some(t) = fresh.take() {
          repeats = 3;
          Some(t)
        } else if (want_key || (repeats > 0 && last_fed.elapsed() > frame_time * 4)) && last.is_some() {
          repeats = repeats.saturating_sub(1);
          last.clone()
        } else {
          None
        };
        if let Some(t) = pick {
          if key.swap(false, Ordering::Relaxed) {
            self.encoder.force_key();
          }
          self.encoder.feed(&t, start.elapsed())?;
          last = Some(t);
          last_fed = Instant::now();
        }
      }
      std::thread::sleep(Duration::from_millis(1));
    }
    Ok(())
  }
}

fn device() -> Result<(ID3D11Device, ID3D11DeviceContext)> {
  let mut dev = None;
  let mut ctx = None;
  unsafe {
    D3D11CreateDevice(
      None,
      D3D_DRIVER_TYPE_HARDWARE,
      HMODULE::default(),
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      None,
      D3D11_SDK_VERSION,
      Some(&mut dev),
      None,
      Some(&mut ctx),
    )?;
  }
  let dev: ID3D11Device = dev.unwrap();
  // The encoder uses the device from its own threads.
  unsafe {
    let _ = dev.cast::<ID3D11Multithread>()?.SetMultithreadProtected(true);
  }
  Ok((dev, ctx.unwrap()))
}

// ---- capture ---------------------------------------------------------------

struct Capture {
  winrt: IDirect3DDevice,
  pool: Direct3D11CaptureFramePool,
  _session: GraphicsCaptureSession,
  size: SizeInt32,
}

impl Capture {
  fn new(dev: &ID3D11Device, hwnd: isize, fps: u32) -> Result<Capture> {
    let winrt: IDirect3DDevice = unsafe { CreateDirect3D11DeviceFromDXGIDevice(&dev.cast::<IDXGIDevice>()?)? }.cast()?;
    let interop = windows::core::factory::<GraphicsCaptureItem, IGraphicsCaptureItemInterop>()?;
    let item: GraphicsCaptureItem = unsafe { interop.CreateForWindow(HWND(hwnd as _))? };
    let size = item.Size()?;
    let pool = Direct3D11CaptureFramePool::CreateFreeThreaded(&winrt, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size)?;
    let session = pool.CreateCaptureSession(&item)?;
    let _ = session.SetIsBorderRequired(false);
    let _ = session.SetIsCursorCaptureEnabled(false);
    // Windows delivers at most ~one image per 16.7 ms by default, which on a
    // 144 Hz monitor rounds down to every third vsync (48 fps). Ask for a
    // little more than the target rate so the encoder sees every frame.
    let interval = 10_000_000 / (fps as i64 + fps as i64 / 4).max(1);
    let _ = session.SetMinUpdateInterval(windows::Foundation::TimeSpan { Duration: interval });
    session.StartCapture()?;
    Ok(Capture { winrt, pool, _session: session, size })
  }

  /// The next captured image, if there is one. Resizes the pool when the
  /// window's size changed.
  fn next(&mut self) -> Result<Option<Direct3D11CaptureFrame>> {
    let Ok(frame) = self.pool.TryGetNextFrame() else { return Ok(None) };
    let cs = frame.ContentSize()?;
    if cs != self.size {
      self.size = cs;
      self.pool.Recreate(&self.winrt, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs)?;
    }
    Ok(Some(frame))
  }
}

// ---- crop + RGB -> NV12 ----------------------------------------------------

struct Converter {
  vctx: ID3D11VideoContext,
  vp: ID3D11VideoProcessor,
  rgb: ID3D11Texture2D,
  rgb_view: ID3D11VideoProcessorInputView,
  /// A few NV12 pictures, used in turn, so the encoder can still be reading
  /// one while the next is written.
  nv12: Vec<(ID3D11Texture2D, ID3D11VideoProcessorOutputView)>,
  next: usize,
  w: u32,
  h: u32,
}

impl Converter {
  fn new(dev: &ID3D11Device, ctx: &ID3D11DeviceContext, w: u32, h: u32) -> Result<Converter> {
    let vdev: ID3D11VideoDevice = dev.cast()?;
    let vctx: ID3D11VideoContext = ctx.cast()?;
    let rate = DXGI_RATIONAL { Numerator: 60, Denominator: 1 };
    let desc = D3D11_VIDEO_PROCESSOR_CONTENT_DESC {
      InputFrameFormat: D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
      InputFrameRate: rate,
      InputWidth: w,
      InputHeight: h,
      OutputFrameRate: rate,
      OutputWidth: w,
      OutputHeight: h,
      Usage: D3D11_VIDEO_USAGE_OPTIMAL_SPEED,
    };
    unsafe {
      let venum = vdev.CreateVideoProcessorEnumerator(&desc)?;
      let vp = vdev.CreateVideoProcessor(&venum, 0)?;
      // Web pages are full-range sRGB; H.264 decoders expect BT.709 video range.
      if let Ok(v1) = vctx.cast::<ID3D11VideoContext1>() {
        v1.VideoProcessorSetStreamColorSpace1(&vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        v1.VideoProcessorSetOutputColorSpace1(&vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
      }
      vctx.VideoProcessorSetStreamFrameFormat(&vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
      vctx.VideoProcessorSetStreamAutoProcessingMode(&vp, 0, false);

      let rgb = texture(dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET.0 as u32)?;
      // Until the first crop lands (or where a crop falls short): white, like a page.
      let mut rtv = None;
      dev.CreateRenderTargetView(&rgb, None, Some(&mut rtv))?;
      ctx.ClearRenderTargetView(&rtv.unwrap(), &[1.0, 1.0, 1.0, 1.0]);
      let in_desc = D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC {
        FourCC: 0,
        ViewDimension: D3D11_VPIV_DIMENSION_TEXTURE2D,
        Anonymous: D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0 { Texture2D: D3D11_TEX2D_VPIV { MipSlice: 0, ArraySlice: 0 } },
      };
      let mut rgb_view = None;
      vdev.CreateVideoProcessorInputView(&rgb, &venum, &in_desc, Some(&mut rgb_view))?;

      let out_desc = D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC {
        ViewDimension: D3D11_VPOV_DIMENSION_TEXTURE2D,
        Anonymous: D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0 { Texture2D: D3D11_TEX2D_VPOV { MipSlice: 0 } },
      };
      let mut nv12 = vec![];
      for _ in 0..6 {
        let t = texture(dev, w, h, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET.0 as u32)?;
        let mut v = None;
        vdev.CreateVideoProcessorOutputView(&t, &venum, &out_desc, Some(&mut v))?;
        nv12.push((t, v.unwrap()));
      }
      Ok(Converter { vctx, vp, rgb, rgb_view: rgb_view.unwrap(), nv12, next: 0, w, h })
    }
  }

  /// Crops the page out of a captured window image and converts it.
  fn run(&mut self, ctx: &ID3D11DeviceContext, src: &ID3D11Texture2D, origin: (i32, i32), size: SizeInt32) -> Result<ID3D11Texture2D> {
    let (ox, oy) = (origin.0.max(0) as u32, origin.1.max(0) as u32);
    let (sw, sh) = (size.Width.max(0) as u32, size.Height.max(0) as u32);
    let b = D3D11_BOX {
      left: ox.min(sw),
      top: oy.min(sh),
      front: 0,
      right: (ox + self.w).min(sw),
      bottom: (oy + self.h).min(sh),
      back: 1,
    };
    let (dst, view) = self.nv12[self.next].clone();
    self.next = (self.next + 1) % self.nv12.len();
    unsafe {
      if b.right > b.left && b.bottom > b.top {
        ctx.CopySubresourceRegion(&self.rgb, 0, 0, 0, 0, src, 0, Some(&b));
      }
      let stream = D3D11_VIDEO_PROCESSOR_STREAM {
        Enable: true.into(),
        pInputSurface: ManuallyDrop::new(Some(self.rgb_view.clone())),
        ..Default::default()
      };
      let mut streams = [stream];
      let r = self.vctx.VideoProcessorBlt(&self.vp, &view, 0, &streams);
      ManuallyDrop::drop(&mut streams[0].pInputSurface);
      r?;
    }
    Ok(dst)
  }
}

fn texture(dev: &ID3D11Device, w: u32, h: u32, format: DXGI_FORMAT, bind: u32) -> Result<ID3D11Texture2D> {
  let desc = D3D11_TEXTURE2D_DESC {
    Width: w,
    Height: h,
    MipLevels: 1,
    ArraySize: 1,
    Format: format,
    SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
    Usage: D3D11_USAGE_DEFAULT,
    BindFlags: bind,
    CPUAccessFlags: 0,
    MiscFlags: 0,
  };
  let mut t = None;
  unsafe { dev.CreateTexture2D(&desc, None, Some(&mut t))? };
  Ok(t.unwrap())
}

// ---- H.264 encoder -----------------------------------------------------------

struct Encoder {
  mft: IMFTransform,
  events: IMFMediaEventGenerator,
  codec: Option<ICodecAPI>,
  _manager: IMFDXGIDeviceManager,
  name: String,
  in_id: u32,
  out_id: u32,
  /// Inputs the encoder has asked for and not yet been given.
  need: u32,
  frame_len: i64,
  /// SPS + PPS, for keyframes that come without them.
  header: Vec<u8>,
}

impl Encoder {
  fn new(dev: &ID3D11Device, s: Settings) -> Result<Encoder> {
    unsafe {
      let input = MFT_REGISTER_TYPE_INFO { guidMajorType: MFMediaType_Video, guidSubtype: MFVideoFormat_NV12 };
      let output = MFT_REGISTER_TYPE_INFO { guidMajorType: MFMediaType_Video, guidSubtype: MFVideoFormat_H264 };
      let mut list: *mut Option<IMFActivate> = std::ptr::null_mut();
      let mut n = 0u32;
      MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        Some(&input),
        Some(&output),
        &mut list,
        &mut n,
      )?;
      let acts: Vec<IMFActivate> =
        if list.is_null() { vec![] } else { std::slice::from_raw_parts_mut(list, n as usize).iter_mut().filter_map(Option::take).collect() };
      if !list.is_null() {
        CoTaskMemFree(Some(list as *const _));
      }
      let Some(act) = acts.into_iter().next() else {
        return Err(windows::core::Error::new(MF_E_NOT_FOUND, "no hardware H.264 encoder"));
      };
      let name = act_name(&act);
      let mft: IMFTransform = act.ActivateObject()?;
      let attrs = mft.GetAttributes()?;
      attrs.SetUINT32(&MF_TRANSFORM_ASYNC_UNLOCK, 1)?;
      let _ = attrs.SetUINT32(&MF_LOW_LATENCY, 1);

      let mut token = 0;
      let mut manager = None;
      MFCreateDXGIDeviceManager(&mut token, &mut manager)?;
      let manager = manager.unwrap();
      manager.ResetDevice(dev, token)?;
      mft.ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, manager.as_raw() as usize)?;

      let (mut in_id, mut out_id) = ([0u32], [0u32]);
      let _ = mft.GetStreamIDs(&mut in_id, &mut out_id);

      // Tuned for interaction, not for film: no B-frames or lookahead (they
      // cost frames of delay), constant bit rate so no frame is a burst
      // that clogs the Wi-Fi, and keyframes only when asked for.
      let codec = mft.cast::<ICodecAPI>().ok();
      if let Some(c) = &codec {
        let set = |api: &windows::core::GUID, v: VARIANT| {
          let _ = c.SetValue(api, &v);
        };
        set(&CODECAPI_AVLowLatencyMode, VARIANT::from(true));
        set(&CODECAPI_AVEncCommonRateControlMode, VARIANT::from(eAVEncCommonRateControlMode_CBR.0 as u32));
        set(&CODECAPI_AVEncCommonMeanBitRate, VARIANT::from(s.bitrate));
        set(&CODECAPI_AVEncMPVDefaultBPictureCount, VARIANT::from(0u32));
        set(&CODECAPI_AVEncMPVGOPSize, VARIANT::from(s.fps * 60));
      }

      let out = MFCreateMediaType()?;
      out.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
      out.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_H264)?;
      out.SetUINT32(&MF_MT_AVG_BITRATE, s.bitrate)?;
      out.SetUINT64(&MF_MT_FRAME_SIZE, ((s.width as u64) << 32) | s.height as u64)?;
      out.SetUINT64(&MF_MT_FRAME_RATE, ((s.fps as u64) << 32) | 1)?;
      out.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, (1u64 << 32) | 1)?;
      out.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
      // Baseline: phone decoders then know no frame is ever reordered and
      // show each one as soon as it's decoded.
      out.SetUINT32(&MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base.0 as u32)?;
      out.SetUINT32(&MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709.0 as u32)?;
      out.SetUINT32(&MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235.0 as u32)?;
      mft.SetOutputType(out_id[0], &out, 0)?;

      let inp = MFCreateMediaType()?;
      inp.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
      inp.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_NV12)?;
      inp.SetUINT64(&MF_MT_FRAME_SIZE, ((s.width as u64) << 32) | s.height as u64)?;
      inp.SetUINT64(&MF_MT_FRAME_RATE, ((s.fps as u64) << 32) | 1)?;
      inp.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, (1u64 << 32) | 1)?;
      inp.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
      mft.SetInputType(in_id[0], &inp, 0)?;

      let header = sequence_header(&mft, out_id[0]);
      let events: IMFMediaEventGenerator = mft.cast()?;
      mft.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)?;
      mft.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)?;
      Ok(Encoder {
        mft,
        events,
        codec,
        _manager: manager,
        name,
        in_id: in_id[0],
        out_id: out_id[0],
        need: 0,
        frame_len: 10_000_000 / s.fps as i64,
        header,
      })
    }
  }

  fn force_key(&self) {
    if let Some(c) = &self.codec {
      unsafe {
        let _ = c.SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &VARIANT::from(1u32));
      }
    }
  }

  /// Changes the bit rate mid-stream (the next frames follow it).
  fn set_bitrate(&self, bps: u32) {
    if let Some(c) = &self.codec {
      unsafe {
        let _ = c.SetValue(&CODECAPI_AVEncCommonMeanBitRate, &VARIANT::from(bps));
      }
    }
  }

  /// Handles what the encoder has to say: requests for input, finished frames.
  fn poll(&mut self, out: &dyn Fn(Vec<u8>), in_flight: &AtomicU32) -> Result<()> {
    loop {
      let ev = match unsafe { self.events.GetEvent(MF_EVENT_FLAG_NO_WAIT) } {
        Ok(ev) => ev,
        Err(e) if e.code() == MF_E_NO_EVENTS_AVAILABLE => return Ok(()),
        Err(e) => return Err(e),
      };
      let kind = unsafe { ev.GetType()? };
      if kind == METransformNeedInput.0 as u32 {
        self.need += 1;
      } else if kind == METransformHaveOutput.0 as u32 {
        if let Some(au) = self.output()? {
          in_flight.fetch_add(1, Ordering::Relaxed);
          out(au);
        }
      }
    }
  }

  fn output(&mut self) -> Result<Option<Vec<u8>>> {
    unsafe {
      let info = self.mft.GetOutputStreamInfo(self.out_id)?;
      let sample = if info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES.0 as u32 != 0 {
        None
      } else {
        let s = MFCreateSample()?;
        s.AddBuffer(&MFCreateMemoryBuffer(info.cbSize.max(1 << 20))?)?;
        Some(s)
      };
      let mut buf = [MFT_OUTPUT_DATA_BUFFER {
        dwStreamID: self.out_id,
        pSample: ManuallyDrop::new(sample),
        dwStatus: 0,
        pEvents: ManuallyDrop::new(None),
      }];
      let mut status = 0;
      let r = self.mft.ProcessOutput(0, &mut buf, &mut status);
      let sample = ManuallyDrop::take(&mut buf[0].pSample);
      drop(ManuallyDrop::take(&mut buf[0].pEvents));
      match r {
        Ok(()) => {}
        Err(e) if e.code() == MF_E_TRANSFORM_STREAM_CHANGE => {
          let t = self.mft.GetOutputAvailableType(self.out_id, 0)?;
          self.mft.SetOutputType(self.out_id, &t, 0)?;
          self.header = sequence_header(&self.mft, self.out_id);
          return Ok(None);
        }
        Err(e) if e.code() == MF_E_TRANSFORM_NEED_MORE_INPUT => return Ok(None),
        Err(e) => return Err(e),
      }
      let Some(sample) = sample else { return Ok(None) };
      let key = sample.GetUINT32(&MFSampleExtension_CleanPoint).unwrap_or(0) != 0;
      let mb = sample.ConvertToContiguousBuffer()?;
      let mut p = std::ptr::null_mut();
      let mut len = 0u32;
      mb.Lock(&mut p, None, Some(&mut len))?;
      let data = std::slice::from_raw_parts(p, len as usize);
      let mut au = Vec::with_capacity(len as usize + self.header.len() + 1);
      au.push(3);
      if key && !has_nal(data, 7) {
        au.extend_from_slice(&self.header);
      }
      au.extend_from_slice(data);
      mb.Unlock()?;
      Ok(Some(au))
    }
  }

  fn feed(&mut self, tex: &ID3D11Texture2D, at: Duration) -> Result<()> {
    unsafe {
      let buf = MFCreateDXGISurfaceBuffer(&ID3D11Texture2D::IID, tex, 0, false)?;
      let len = buf.cast::<IMF2DBuffer>()?.GetContiguousLength()?;
      buf.SetCurrentLength(len)?;
      let s = MFCreateSample()?;
      s.AddBuffer(&buf)?;
      s.SetSampleTime((at.as_nanos() / 100) as i64)?;
      s.SetSampleDuration(self.frame_len)?;
      self.mft.ProcessInput(self.in_id, &s, 0)?;
    }
    self.need -= 1;
    Ok(())
  }
}

impl Drop for Encoder {
  fn drop(&mut self) {
    unsafe {
      let _ = self.mft.ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      let _ = self.mft.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
  }
}

fn act_name(act: &IMFActivate) -> String {
  unsafe {
    let mut p = windows::core::PWSTR::null();
    let mut n = 0;
    if act.GetAllocatedString(&MFT_FRIENDLY_NAME_Attribute, &mut p, &mut n).is_ok() && !p.is_null() {
      let s = p.to_string().unwrap_or_default();
      CoTaskMemFree(Some(p.0 as *const _));
      return s;
    }
  }
  "hardware encoder".into()
}

fn sequence_header(mft: &IMFTransform, out_id: u32) -> Vec<u8> {
  unsafe {
    let Ok(t) = mft.GetOutputCurrentType(out_id) else { return vec![] };
    let Ok(n) = t.GetBlobSize(&MF_MT_MPEG_SEQUENCE_HEADER) else { return vec![] };
    let mut v = vec![0u8; n as usize];
    if t.GetBlob(&MF_MT_MPEG_SEQUENCE_HEADER, &mut v, None).is_err() {
      return vec![];
    }
    v
  }
}

/// Whether an Annex B stream contains a NAL unit of this type.
fn has_nal(data: &[u8], kind: u8) -> bool {
  data.windows(4).any(|w| w[0] == 0 && w[1] == 0 && w[2] == 1 && w[3] & 0x1f == kind)
}

#[cfg(test)]
mod tests {
  use super::has_nal;

  #[test]
  fn finds_sps() {
    assert!(has_nal(&[0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68], 7));
    assert!(!has_nal(&[0, 0, 0, 1, 0x65, 1, 2], 7));
  }
}
