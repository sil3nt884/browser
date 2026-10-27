namespace blink {
class MultiBufferDataSource {
 private:
  // Preload this many seconds of data by default.
  media::Tuneable<int> preload_seconds_ = {"SrcMediaMultiBufferPreloadSeconds",
                                           0, 10, 60};

  // Keep this many seconds of data for going back by default.
  media::Tuneable<int> keep_after_playback_seconds_ = {
      "SrcMediaMultiBufferKeepAfterPlaybackSeconds", 0, 2, 60};
};
}
