// Copyright 2026 The Umbra Authors.
//! Decodes an Annex B H.264 file with Windows' own decoder and saves one
//! frame's brightness (Y plane) as a BMP, to check what the agent encodes.
//!
//!   cargo run --release --example h264_check -- <in.h264> <frame index> <out.bmp>

use std::mem::ManuallyDrop;

use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoCreateInstance, CoInitializeEx, CLSCTX_INPROC_SERVER};

fn main() -> windows::core::Result<()> {
  let a: Vec<String> = std::env::args().collect();
  let data = std::fs::read(&a[1]).unwrap();
  let want: usize = a[2].parse().unwrap();
  // Split at access unit delimiters (NAL type 9), which NVENC writes first.
  let mut starts = vec![];
  for i in 0..data.len().saturating_sub(4) {
    if data[i..i + 3] == [0, 0, 1] && data[i + 3] & 0x1f == 9 {
      starts.push(if i > 0 && data[i - 1] == 0 { i - 1 } else { i });
    }
  }
  starts.push(data.len());
  println!("{} access units", starts.len() - 1);
  unsafe {
    CoInitializeEx(None, COINIT_MULTITHREADED).ok()?;
    MFStartup(MF_VERSION, MFSTARTUP_LITE)?;
    let dec: IMFTransform = CoCreateInstance(&CLSID_MSH264DecoderMFT, None, CLSCTX_INPROC_SERVER)?;
    let t = MFCreateMediaType()?;
    t.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
    t.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_H264)?;
    dec.SetInputType(0, &t, 0)?;
    let o = dec.GetOutputAvailableType(0, 0)?;
    dec.SetOutputType(0, &o, 0)?;
    let (mut frames, mut size) = (0usize, (0u32, 0u32));
    for w in starts.windows(2) {
      let au = &data[w[0]..w[1]];
      let buf = MFCreateMemoryBuffer(au.len() as u32)?;
      let mut p = std::ptr::null_mut();
      buf.Lock(&mut p, None, None)?;
      std::ptr::copy_nonoverlapping(au.as_ptr(), p, au.len());
      buf.Unlock()?;
      buf.SetCurrentLength(au.len() as u32)?;
      let s = MFCreateSample()?;
      s.AddBuffer(&buf)?;
      dec.ProcessInput(0, &s, 0)?;
      loop {
        let info = dec.GetOutputStreamInfo(0)?;
        let os = MFCreateSample()?;
        os.AddBuffer(&MFCreateMemoryBuffer(info.cbSize.max(1))?)?;
        let mut b = [MFT_OUTPUT_DATA_BUFFER { dwStreamID: 0, pSample: ManuallyDrop::new(Some(os)), dwStatus: 0, pEvents: ManuallyDrop::new(None) }];
        let mut st = 0;
        let r = dec.ProcessOutput(0, &mut b, &mut st);
        let os = ManuallyDrop::take(&mut b[0].pSample).unwrap();
        drop(ManuallyDrop::take(&mut b[0].pEvents));
        match r {
          Ok(()) => {
            if frames == want {
              let mb = os.ConvertToContiguousBuffer()?;
              let mut p = std::ptr::null_mut();
              let mut len = 0;
              mb.Lock(&mut p, None, Some(&mut len))?;
              let y = std::slice::from_raw_parts(p, (size.0 * size.1) as usize);
              save(y, size.0 as usize, size.1 as usize, &a[3]);
              mb.Unlock()?;
            }
            frames += 1;
          }
          Err(e) if e.code() == MF_E_TRANSFORM_STREAM_CHANGE => {
            let o = dec.GetOutputAvailableType(0, 0)?;
            let fs = o.GetUINT64(&MF_MT_FRAME_SIZE)?;
            size = ((fs >> 32) as u32, fs as u32);
            println!("decoder output {}x{}", size.0, size.1);
            dec.SetOutputType(0, &o, 0)?;
          }
          Err(e) if e.code() == MF_E_TRANSFORM_NEED_MORE_INPUT => break,
          Err(e) => return Err(e),
        }
      }
    }
    println!("{frames} frames decoded");
  }
  Ok(())
}

fn save(y: &[u8], w: usize, h: usize, out: &str) {
  // The decoder pads rows to a multiple of 16 lines; the stride is w here.
  let row = (w * 3 + 3) & !3;
  let mut f = Vec::new();
  let size = (54 + row * h) as u32;
  f.extend_from_slice(b"BM");
  for v in [size, 0, 54, 40, w as u32, h as u32] {
    f.extend_from_slice(&v.to_le_bytes());
  }
  f.extend_from_slice(&1u16.to_le_bytes());
  f.extend_from_slice(&24u16.to_le_bytes());
  for v in [0u32, (row * h) as u32, 2835, 2835, 0, 0] {
    f.extend_from_slice(&v.to_le_bytes());
  }
  for r in (0..h).rev() {
    let start = f.len();
    for &v in &y[r * w..r * w + w] {
      f.extend_from_slice(&[v, v, v]);
    }
    f.resize(start + row, 0);
  }
  std::fs::write(out, f).unwrap();
  println!("saved frame Y plane to {out}");
}
