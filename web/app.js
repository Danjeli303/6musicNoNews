const form = document.querySelector("#programme-form");
const input = document.querySelector("#sounds-url");
const submitButton = document.querySelector("#submit-button");
const card = document.querySelector("#job-card");
const kicker = document.querySelector("#job-kicker");
const stage = document.querySelector("#job-stage");
const progressNumber = document.querySelector("#progress-number");
const progressTrack = document.querySelector("#progress-track");
const progressBar = document.querySelector("#progress-bar");
const message = document.querySelector("#job-message");
const logs = document.querySelector("#job-logs");
const downloadButton = document.querySelector("#download-button");
const activeJobs = document.querySelector("#active-jobs");
const historyJobs = document.querySelector("#history-jobs");
const activeCount = document.querySelector("#active-count");
const historyCount = document.querySelector("#history-count");
const activeEmpty = document.querySelector("#active-empty");
const historyEmpty = document.querySelector("#history-empty");
const libraryMessage = document.querySelector("#library-message");
const refreshButton = document.querySelector("#refresh-jobs");
const radioStatus = document.querySelector("#radio-status");
const liveSection = document.querySelector("#live-stream");
const liveDescription = document.querySelector("#live-description");
const liveOnAir = document.querySelector("#live-on-air");
const nowPlayingLabel = document.querySelector("#now-playing-label");
const nowPlayingTitle = document.querySelector("#now-playing-title");
const nowPlayingArtist = document.querySelector("#now-playing-artist");
const nowPlayingImage = document.querySelector("#now-playing-image");
const nowPlayingImageFallback = document.querySelector(
  "#now-playing-image-fallback",
);
const nowPlayingFavourite = document.querySelector("#now-playing-favourite");
const headerRadioToggle = document.querySelector("#header-radio-toggle");
const liveNowPlayingLabel = document.querySelector("#live-now-playing-label");
const liveNowPlayingTitle = document.querySelector("#live-now-playing-title");
const liveNowPlayingArtist = document.querySelector("#live-now-playing-artist");
const liveNowPlayingImage = document.querySelector("#live-now-playing-image");
const liveNowPlayingImageFallback = document.querySelector(
  "#live-now-playing-image-fallback",
);
const liveNowPlayingFavourite = document.querySelector(
  "#live-now-playing-favourite",
);
const liveProgrammeImage = document.querySelector("#live-programme-image");
const liveProgrammeImageFallback = document.querySelector(
  "#live-programme-image-fallback",
);
const fipToggle = document.querySelector("#fip-toggle");
const fipToggleStatus = document.querySelector("#fip-toggle-status");
const siteIcon = document.querySelector("#site-icon");
const defaultSiteIcon = siteIcon?.href;

let pollTimer;
let displayedJobId;
let libraryRequestInFlight = false;
let radioPlayer;
let historySignature = "";
let nowPlayingSignature = "";
let nowPlayingRequestInFlight = false;
let nowPlayingQueueTimer;
let nowPlayingHasTrack = false;
let currentLiveTrack;
let currentLiveProgramme;
let pendingNowPlayingSignature = "";
let activeMediaPlayer;
let activeHeaderEntry;
let showingProgrammeContext = false;
let fipToggleCooldownTimer;
let fipToggleCooldownUntil = 0;
let fipMixerTransitioning = false;
const historyPlayers = new Map();

function updateHeaderRadioToggle(isPlaying, source = "live radio") {
  if (!headerRadioToggle) return;
  headerRadioToggle.classList.toggle("is-playing", isPlaying);
  headerRadioToggle.querySelector("span").textContent = isPlaying ? "■" : "▶";
  const action = isPlaying ? "Stop" : "Play";
  headerRadioToggle.title = `${action} ${source}`;
  headerRadioToggle.setAttribute("aria-label", `${action} ${source}`);
}

function alternateHeaderMetadata() {
  if (activeHeaderEntry && activeMediaPlayer === activeHeaderEntry.player) {
    showingProgrammeContext = !showingProgrammeContext;
    renderHeaderHistory(activeHeaderEntry, showingProgrammeContext);
    return;
  }
  if (!currentLiveTrack && !currentLiveProgramme?.available) return;
  showingProgrammeContext = !showingProgrammeContext;
  if (showingProgrammeContext) {
    renderHeaderProgramme(currentLiveProgramme);
  } else if (currentLiveTrack) {
    renderNowPlaying(currentLiveTrack, true);
  }
}

function mediaSessionAvailable() {
  return (
    "mediaSession" in navigator &&
    typeof window.MediaMetadata === "function"
  );
}

function mediaArtwork(track, fallbackArtwork) {
  const source = track?.image_url || fallbackArtwork;
  if (!source) return [];
  try {
    return [{ src: new URL(source, window.location.href).href }];
  } catch (_error) {
    return [];
  }
}

function mediaAlbum(track, fallbackAlbum, fallbackPresenter) {
  const programme =
    track?.programme || fallbackAlbum || track?.station || "BBC Radio 6 Music";
  const presenter = track?.presenter || fallbackPresenter;
  return presenter && presenter !== programme
    ? `${programme} · ${presenter}`
    : programme;
}

function updateSystemMediaMetadata(track, options = {}) {
  if (!mediaSessionAvailable() || (!track && !options.title)) return;
  const item = track || {};
  const station = item.station || item.source || "BBC Radio 6 Music";
  try {
    navigator.mediaSession.metadata = new MediaMetadata({
      title: item.title || item.name || options.title || station,
      artist:
        item.artist?.name ||
        item.artist ||
        options.artist ||
        station,
      album: mediaAlbum(item, options.album, options.presenter),
      artwork: mediaArtwork(item, options.artwork),
    });
  } catch (_error) {
    // Safari versions without full MediaMetadata support retain native controls.
  }
}

function updateSystemPlaybackState(state) {
  if (!mediaSessionAvailable()) return;
  try {
    navigator.mediaSession.playbackState = state;
  } catch (_error) {
    // Some older Safari releases expose Media Session without playbackState.
  }
}

function clearSystemPositionState() {
  if (!mediaSessionAvailable() || !navigator.mediaSession.setPositionState) return;
  try {
    navigator.mediaSession.setPositionState();
  } catch (_error) {
    // Position state is optional and is not available in every Safari release.
  }
}

function updateSystemPositionState(player) {
  if (!mediaSessionAvailable() || !navigator.mediaSession.setPositionState) return;
  const duration = Number(player.duration());
  const position = Number(player.currentTime());
  const playbackRate = Number(player.playbackRate());
  if (!Number.isFinite(duration) || duration <= 0 || !Number.isFinite(position)) {
    return;
  }
  try {
    navigator.mediaSession.setPositionState({
      duration,
      playbackRate: Number.isFinite(playbackRate) ? playbackRate : 1,
      position: Math.max(0, Math.min(duration, position)),
    });
  } catch (_error) {
    // Lock-screen seeking remains optional when the browser rejects this state.
  }
}

function showArtworkFallback(image, fallback) {
  image.hidden = true;
  image.removeAttribute("src");
  image.alt = "";
  fallback.hidden = false;
}

function showArtwork(image, fallback, source, alt) {
  if (!source) {
    showArtworkFallback(image, fallback);
    return;
  }
  image.onload = () => {
    image.hidden = false;
    fallback.hidden = true;
  };
  image.onerror = () => showArtworkFallback(image, fallback);
  image.alt = alt;
  image.src = source;
}

function updateSiteIcon(source) {
  if (!siteIcon) return;
  siteIcon.href = source || defaultSiteIcon;
}

function setTrackLink(element, track, fallback) {
  element.textContent = track?.title || track?.name || fallback;
  if (track && (track.title || track.name)) {
    element.href = window.SkipperFavourites.webSearchUrl(track);
    element.target = "_blank";
    element.rel = "noopener";
  } else {
    element.removeAttribute("href");
    element.removeAttribute("target");
    element.removeAttribute("rel");
  }
}

function setProgrammeLink(element, programme, fallback) {
  element.textContent = programme?.title || fallback;
  if (programme?.url) {
    element.href = programme.url;
    element.target = "_blank";
    element.rel = "noopener";
  } else {
    element.removeAttribute("href");
    element.removeAttribute("target");
    element.removeAttribute("rel");
  }
}

function programmeDescription(programme) {
  return programme?.subtitle || programme?.presenter || "Live radio";
}

function stationName(track = currentLiveTrack) {
  return track?.station || track?.source || "BBC Radio 6 Music";
}

function setLiveStationIdentity(track) {
  const isFip = stationName(track) === "FIP";
  liveSection?.classList.toggle("is-fip", isFip);
  document.body.classList.toggle("is-fip-live", isFip);
  if (liveDescription) {
    liveDescription.textContent = isFip
      ? "FIP while scheduled BBC Radio 6 Music news is being replaced."
      : "BBC Radio 6 Music without scheduled news.";
  }
  if (liveOnAir) liveOnAir.lastChild.textContent = isFip ? " FIP on air" : " Live now";

  const stationStrong = liveProgrammeImageFallback?.querySelector("strong");
  const stationLabel = liveProgrammeImageFallback?.querySelector("b");
  if (stationStrong) stationStrong.textContent = isFip ? "FIP" : "6";
  if (stationLabel) stationLabel.textContent = isFip ? "RADIO FRANCE" : "MUSIC";
  liveProgrammeImageFallback?.classList.toggle("is-fip", isFip);
  if (liveNowPlayingImageFallback) {
    liveNowPlayingImageFallback.textContent = isFip ? "FIP" : "6";
  }
  if (nowPlayingImageFallback) {
    nowPlayingImageFallback.textContent = isFip ? "FIP" : "6";
  }
}

function liveProgrammeStatus() {
  if (!currentLiveProgramme?.available) return `Playing ${stationName()} live.`;
  const detail = programmeDescription(currentLiveProgramme);
  return detail && detail !== currentLiveProgramme.title
    ? `${currentLiveProgramme.title} · ${detail}`
    : currentLiveProgramme.title;
}

function renderHeaderProgramme(programme) {
  showingProgrammeContext = true;
  nowPlayingLabel.textContent = "Live programme";
  setProgrammeLink(nowPlayingTitle, programme, stationName());
  nowPlayingArtist.textContent = programmeDescription(programme);
  showArtwork(
    nowPlayingImage,
    nowPlayingImageFallback,
    programme?.image_url,
    `Artwork for ${programme?.title || stationName()}`,
  );
  updateFavouriteButton(nowPlayingFavourite, undefined);
}

function renderLiveNowPlaying(track) {
  const station = stationName(track);
  if (!track.available) {
    liveNowPlayingLabel.textContent = `Now playing on ${station}`;
    setTrackLink(liveNowPlayingTitle, undefined, "Track information unavailable");
    liveNowPlayingArtist.textContent = station;
    showArtworkFallback(liveNowPlayingImage, liveNowPlayingImageFallback);
    updateFavouriteButton(liveNowPlayingFavourite, undefined);
    return;
  }
  liveNowPlayingLabel.textContent = track.now_playing
    ? `Now playing on ${station}`
    : `Recently played on ${station}`;
  setTrackLink(liveNowPlayingTitle, track, "Title unavailable");
  liveNowPlayingArtist.textContent = track.artist || station;
  showArtwork(
    liveNowPlayingImage,
    liveNowPlayingImageFallback,
    track.image_url,
    `Artwork for ${liveNowPlayingArtist.textContent} – ${liveNowPlayingTitle.textContent}`,
  );
  updateFavouriteButton(liveNowPlayingFavourite, track);
}

function renderHeaderHistory(entry, programmeOnly = false) {
  const track = programmeOnly ? undefined : entry.currentTrack;
  showingProgrammeContext = programmeOnly;
  nowPlayingLabel.textContent = track ? "Playing track" : "Playing programme";
  setTrackLink(nowPlayingTitle, track, entry.album || entry.title);
  nowPlayingArtist.textContent = track?.artist || entry.presenter || entry.artist || "BBC Sounds";
  showArtwork(
    nowPlayingImage,
    nowPlayingImageFallback,
    track?.image_url || entry.artwork,
    `Artwork for ${entry.title}`,
  );
  updateFavouriteButton(nowPlayingFavourite, track);
  updateHeaderRadioToggle(!entry.player.paused(), entry.title);
}

function showNowPlayingFallback() {
  showArtworkFallback(nowPlayingImage, nowPlayingImageFallback);
}

function nowPlayingTrackSignature(track) {
  return JSON.stringify([
    track.available,
    track.now_playing,
    track.artist,
    track.title,
    track.image_url,
    track.programme,
    track.programme_pid,
    track.programme_image_url,
    track.show?.subtitle,
    track.source,
    track.news_active,
  ]);
}

function renderNowPlaying(track, force = false) {
  const signature = nowPlayingTrackSignature(track);
  if (!force && signature === nowPlayingSignature) return;
  nowPlayingSignature = signature;
  showingProgrammeContext = false;
  setLiveStationIdentity(track);
  currentLiveProgramme = track.show?.available ? track.show : undefined;
  showArtwork(
    liveProgrammeImage,
    liveProgrammeImageFallback,
    currentLiveProgramme?.image_url,
    `Artwork for ${currentLiveProgramme?.title || stationName(track)}`,
  );
  updateSiteIcon(currentLiveProgramme?.image_url);

  if (!track.available) {
    nowPlayingHasTrack = false;
    currentLiveTrack = track;
    renderLiveNowPlaying(track);
    if (currentLiveProgramme) {
      if (!(activeHeaderEntry && activeMediaPlayer === activeHeaderEntry.player)) {
        renderHeaderProgramme(currentLiveProgramme);
      }
      return;
    }
    if (activeHeaderEntry && activeMediaPlayer === activeHeaderEntry.player) return;
    nowPlayingLabel.textContent = "Now playing";
    nowPlayingTitle.textContent = "Track information unavailable";
    nowPlayingArtist.textContent = stationName(track);
    showNowPlayingFallback();
    updateFavouriteButton(nowPlayingFavourite, undefined);
    return;
  }

  nowPlayingHasTrack = true;
  currentLiveTrack = track;
  renderLiveNowPlaying(track);
  if (activeHeaderEntry && activeMediaPlayer === activeHeaderEntry.player) return;
  nowPlayingLabel.textContent = track.now_playing
    ? `Now playing on ${stationName(track)}`
    : `Recently played on ${stationName(track)}`;
  setTrackLink(nowPlayingTitle, track, "Title unavailable");
  nowPlayingArtist.textContent = track.artist || stationName(track);
  showArtwork(
    nowPlayingImage,
    nowPlayingImageFallback,
    track.image_url,
    `Artwork for ${nowPlayingArtist.textContent} – ${nowPlayingTitle.textContent}`,
  );
  updateFavouriteButton(nowPlayingFavourite, track);
  if (activeMediaPlayer === radioPlayer) {
    updateSystemMediaMetadata(track);
  }
  if (activeMediaPlayer === radioPlayer && !radioPlayer.paused()) {
    radioStatus.textContent = liveProgrammeStatus();
  }
}

function updateFavouriteButton(button, track) {
  if (!button) return;
  const available = Boolean(track && (track.title || track.name));
  const isFavourite = available && window.SkipperFavourites.has(track);
  button.disabled = !available;
  button.classList.toggle("is-favourite", isFavourite);
  button.querySelector("span").textContent = isFavourite ? "♥" : "♡";
  const action = isFavourite ? "Remove from" : "Add to";
  const description = track
    ? `${track.title || track.name} by ${track.artist?.name || track.artist || "unknown artist"}`
    : "the current track";
  button.title = `${action} favourites`;
  button.setAttribute("aria-label", `${action} favourites: ${description}`);
}

function queueNowPlaying(track) {
  if (
    !track.available &&
    nowPlayingHasTrack &&
    track.source === currentLiveTrack?.source
  ) {
    return;
  }

  const signature = nowPlayingTrackSignature(track);
  if (!nowPlayingHasTrack) {
    renderNowPlaying(track);
    return;
  }
  if (signature === nowPlayingSignature) {
    clearTimeout(nowPlayingQueueTimer);
    pendingNowPlayingSignature = "";
    return;
  }
  if (signature === pendingNowPlayingSignature) {
    return;
  }

  const configuredDelay = Number(track.display_delay_seconds);
  const delaySeconds = Number.isFinite(configuredDelay)
    ? Math.max(0, Math.min(120, configuredDelay))
    : 18;
  clearTimeout(nowPlayingQueueTimer);
  pendingNowPlayingSignature = signature;
  nowPlayingQueueTimer = setTimeout(() => {
    pendingNowPlayingSignature = "";
    renderNowPlaying(track);
  }, delaySeconds * 1000);
}

function setFipToggleState(enabled) {
  if (!fipToggle) return;
  fipToggle.setAttribute("aria-checked", enabled ? "true" : "false");
  fipToggle.setAttribute(
    "aria-label",
    enabled
      ? "Return to BBC Radio 6 Music"
      : "Play FIP instead of BBC Radio 6 Music",
  );
}

function startFipToggleCooldown() {
  fipToggleCooldownUntil = Date.now() + 30000;
  clearInterval(fipToggleCooldownTimer);

  const updateCooldown = () => {
    const remainingSeconds = Math.ceil(
      (fipToggleCooldownUntil - Date.now()) / 1000,
    );
    if (remainingSeconds <= 0) {
      clearInterval(fipToggleCooldownTimer);
      fipToggleCooldownTimer = undefined;
      fipToggleCooldownUntil = 0;
      updateFipToggleAvailability();
      loadNowPlaying();
      return;
    }
    updateFipToggleAvailability();
    fipToggleStatus.textContent = `Switching · ${remainingSeconds}s`;
  };

  updateCooldown();
  fipToggleCooldownTimer = setInterval(updateCooldown, 1000);
}

function updateFipToggleAvailability() {
  if (!fipToggle) return;
  const coolingDown = Date.now() < fipToggleCooldownUntil;
  fipToggle.disabled = coolingDown || fipMixerTransitioning;
  if (!coolingDown) {
    fipToggleStatus.textContent = fipMixerTransitioning ? "Switching…" : "";
  }
}

async function toggleFip() {
  if (!fipToggle || fipToggle.disabled) return;
  const enabled = fipToggle.getAttribute("aria-checked") !== "true";
  const previousState = !enabled;
  fipToggle.disabled = true;
  fipToggleStatus.textContent = "Switching…";
  try {
    const response = await fetch("/api/fip-toggle", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ enabled }),
    });
    await readResponse(response);
    startFipToggleCooldown();
  } catch (error) {
    setFipToggleState(previousState);
    fipToggleStatus.textContent = error.message || "Unavailable";
    fipToggle.disabled = false;
  }
}

async function loadNowPlaying() {
  if (nowPlayingRequestInFlight) return;
  nowPlayingRequestInFlight = true;
  try {
    const response = await fetch("/api/now-playing", { cache: "no-store" });
    const track = await readResponse(response);
    fipMixerTransitioning = track.news?.transitioning === true;
    setFipToggleState(track.news?.news_active === true);
    updateFipToggleAvailability();
    queueNowPlaying(track);
  } catch (_error) {
    if (!nowPlayingHasTrack) renderNowPlaying({ available: false });
  } finally {
    nowPlayingRequestInFlight = false;
  }
}

function stopHistoryPlayer(entry) {
  if (!entry) return;
  entry.player.pause();
  try {
    entry.player.currentTime(0);
  } catch (_error) {
    // The player may not have loaded its metadata yet.
  }
  entry.button.classList.remove("is-playing");
  entry.button.setAttribute("aria-label", `Play ${entry.title}`);
  entry.status.textContent = "Stopped.";
}

function enforceExclusivePlayback(currentPlayer) {
  if (radioPlayer && radioPlayer !== currentPlayer && !radioPlayer.paused()) {
    radioPlayer.pause();
  }
  historyPlayers.forEach((entry) => {
    if (entry.player !== currentPlayer && !entry.player.paused()) {
      stopHistoryPlayer(entry);
    }
  });
}

function initialiseRadioPlayer() {
  if (typeof window.videojs !== "function") {
    radioStatus.textContent = "The radio player could not be loaded.";
    return;
  }

  radioPlayer = window.videojs("live-radio-player", {
    audioOnlyMode: true,
    autoplay: false,
    controls: true,
    liveui: true,
    preload: "none",
    responsive: true,
  });

  radioPlayer.on("play", () => {
    enforceExclusivePlayback(radioPlayer);
    activeMediaPlayer = radioPlayer;
    activeHeaderEntry = undefined;
    if (currentLiveTrack) renderNowPlaying(currentLiveTrack, true);
    updateSystemMediaMetadata(currentLiveTrack);
    clearSystemPositionState();
    updateSystemPlaybackState("playing");
    radioStatus.textContent = "Connecting to the live stream…";
    updateHeaderRadioToggle(true);
  });
  radioPlayer.on("playing", () => {
    updateSystemPlaybackState("playing");
    radioStatus.textContent = liveProgrammeStatus();
  });
  radioPlayer.on("waiting", () => {
    radioStatus.textContent = "Reconnecting to the live stream…";
  });
  radioPlayer.on("pause", () => {
    if (activeMediaPlayer === radioPlayer) {
      updateSystemPlaybackState("paused");
    }
    radioStatus.textContent = "Live stream paused.";
    updateHeaderRadioToggle(false);
  });
  radioPlayer.on("error", () => {
    if (activeMediaPlayer === radioPlayer) {
      updateSystemPlaybackState("none");
    }
    radioStatus.textContent =
      "The live stream is temporarily unavailable. Try again shortly.";
  });
}

function formatDate(value) {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "Date unavailable";
  return new Intl.DateTimeFormat(undefined, {
    dateStyle: "medium",
    timeStyle: "short",
  }).format(date);
}

function formatBytes(value) {
  const bytes = Number(value);
  if (!Number.isFinite(bytes) || bytes < 0) return "";
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB"];
  let size = bytes / 1024;
  let unit = units[0];
  for (let index = 1; size >= 1024 && index < units.length; index += 1) {
    size /= 1024;
    unit = units[index];
  }
  return `${size.toFixed(size >= 10 ? 1 : 2)} ${unit}`;
}

function makeElement(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

function statusText(job) {
  if (job.status === "complete") return "Ready";
  if (job.status === "failed") return "Failed";
  if (job.status === "queued") return "Queued";
  return "Processing";
}

function createProcessItem(job, isActive) {
  const item = makeElement("article", "process-item");
  item.dataset.status = job.status;

  if (!isActive) {
    item.classList.add("processed-item");
    const title =
      job.display_title || job.title || job.filename || "BBC Sounds programme";
    const artwork = makeElement("div", "programme-artwork");
    if (job.artwork_url) {
      const image = makeElement("img");
      image.src = job.artwork_url;
      image.alt = `${title} artwork`;
      image.loading = "lazy";
      artwork.append(image);
    } else {
      const fallback = makeElement("div", "artwork-fallback");
      fallback.setAttribute("aria-hidden", "true");
      fallback.append(makeElement("b", "", "6"), makeElement("span", "", "MUSIC"));
      artwork.append(fallback);
    }

    if (job.status === "complete" && job.media_url) {
      const playButton = makeElement("button", "artwork-play");
      playButton.type = "button";
      playButton.dataset.playJobId = job.id;
      playButton.setAttribute("aria-label", `Play ${title}`);
      artwork.append(playButton);
    }

    const programme = makeElement("div", "programme-details");
    const heading = makeElement("div", "programme-heading");
    heading.append(
      makeElement("h4", "", title),
      makeElement("span", "job-status", statusText(job)),
    );
    programme.append(heading);

    if (job.description) {
      programme.append(makeElement("p", "programme-description", job.description));
    }

    const metaParts = [];
    if (job.album) metaParts.push(`Album: ${job.album}`);
    if (job.created_at) metaParts.push(formatDate(job.created_at));
    const size = formatBytes(job.file_size);
    if (size) metaParts.push(size);
    if (job.pid) metaParts.push(job.pid);
    programme.append(makeElement("p", "programme-meta", metaParts.join(" · ")));

    if (job.status === "complete" && job.media_url) {
      const playerWrap = makeElement("div", "history-player-wrap");
      const trackInfo = makeElement("div", "offline-now-playing");
      trackInfo.dataset.trackInfoId = job.id;
      trackInfo.hidden = true;
      const trackCopy = makeElement("div", "offline-track-copy");
      trackCopy.append(
        makeElement("p", "offline-track-label", "Playing track"),
        makeElement("a", "offline-track-title track-search-link", ""),
        makeElement("span", "offline-track-artist", ""),
      );
      const favourite = makeElement("button", "favourite-button");
      favourite.type = "button";
      favourite.dataset.trackFavouriteId = job.id;
      favourite.append(makeElement("span", "", "♡"));
      trackInfo.append(trackCopy, favourite);
      const audio = makeElement("audio", "video-js history-audio");
      audio.id = `history-audio-${job.id.replace(/[^a-zA-Z0-9_-]/g, "-")}`;
      audio.controls = true;
      audio.preload = "none";
      const source = makeElement("source");
      source.src = job.media_url;
      source.type = job.media_type || "audio/mp4";
      audio.append(source);
      const playerStatus = makeElement(
        "p",
        "history-player-status",
        "Ready to play.",
      );
      playerStatus.dataset.playerStatusId = job.id;
      playerWrap.append(trackInfo, audio, playerStatus);
      programme.append(playerWrap);
    }

    programme.append(
      makeElement(
        "p",
        "process-filename",
        job.filename || job.message || "No output file",
      ),
    );
    const actions = makeElement("div", "process-actions");
    if (job.status === "complete" && job.download_url) {
      const download = makeElement("a", "library-download", "Download");
      download.href = job.download_url;
      download.setAttribute("download", job.filename || "news-skipped-programme");
      actions.append(download);
    }
    const remove = makeElement("button", "remove-button", "Remove");
    remove.type = "button";
    remove.dataset.jobId = job.id;
    remove.dataset.jobName = title;
    actions.append(remove);
    programme.append(actions);
    item.append(artwork, programme);
    return item;
  }

  const summary = makeElement("div", "process-summary");
  const details = makeElement("div", "process-copy");
  details.append(
    makeElement("h4", "", job.title || job.filename || "BBC Sounds programme"),
  );

  const metaParts = [];
  if (job.pid) metaParts.push(job.pid);
  const size = formatBytes(job.file_size);
  if (size) metaParts.push(size);
  details.append(makeElement("p", "process-meta", metaParts.join(" · ")));
  summary.append(details, makeElement("span", "job-status", statusText(job)));
  item.append(summary);

  const progress = Math.max(0, Math.min(100, Number(job.progress) || 0));
  const progressLine = makeElement("div", "compact-progress-line");
  progressLine.append(
    makeElement("span", "", job.stage_label || "Waiting to start"),
    makeElement("b", "", `${progress}%`),
  );
  const track = makeElement("div", "compact-progress");
  track.setAttribute("role", "progressbar");
  track.setAttribute("aria-valuemin", "0");
  track.setAttribute("aria-valuemax", "100");
  track.setAttribute("aria-valuenow", String(progress));
  const bar = makeElement("span");
  bar.style.width = `${progress}%`;
  track.append(bar);
  item.append(progressLine, track);
  return item;
}

function disposeHistoryPlayers() {
  historyPlayers.forEach((entry) => entry.player.dispose());
  historyPlayers.clear();
}

function initialiseHistoryPlayers(history) {
  if (typeof window.videojs !== "function") return;
  history.forEach((job) => {
    if (job.status !== "complete" || !job.media_url) return;
    const audioId = `history-audio-${job.id.replace(/[^a-zA-Z0-9_-]/g, "-")}`;
    const button = historyJobs.querySelector(`[data-play-job-id="${job.id}"]`);
    const status = historyJobs.querySelector(
      `[data-player-status-id="${job.id}"]`,
    );
    const trackInfo = historyJobs.querySelector(`[data-track-info-id="${job.id}"]`);
    const favourite = historyJobs.querySelector(
      `[data-track-favourite-id="${job.id}"]`,
    );
    if (!button || !status || !trackInfo || !favourite || !document.getElementById(audioId)) return;

    const title = job.display_title || job.title || job.filename || "programme";
    const player = window.videojs(audioId, {
      audioOnlyMode: true,
      autoplay: false,
      controls: true,
      preload: "none",
      responsive: true,
    });
    const entry = {
      player,
      button,
      status,
      title,
      trackInfo,
      favourite,
      tracks: Array.isArray(job.tracks) ? job.tracks : [],
      artwork: job.artwork_url,
      album: job.album,
      presenter: job.title,
      artist: job.artist,
      currentTrack: undefined,
      trackSignature: "",
    };
    historyPlayers.set(job.id, entry);

    const renderPlayingTrack = () => {
      const currentTime = player.currentTime();
      const track = entry.tracks.find((candidate) => {
        const start = Number(candidate.start_seconds);
        const end = Number(candidate.end_seconds);
        return Number.isFinite(start) && currentTime >= start && (!Number.isFinite(end) || currentTime < end);
      });
      const signature = track
        ? JSON.stringify([track.artist, track.title, track.start_seconds])
        : "";
      if (signature === entry.trackSignature) return;
      entry.trackSignature = signature;
      entry.currentTrack = track;
      entry.trackInfo.hidden = !track;
      if (!track) {
        if (activeMediaPlayer === player) {
          updateSystemMediaMetadata(undefined, entry);
          renderHeaderHistory(entry, true);
        }
        return;
      }
      const trackTitle = entry.trackInfo.querySelector(".offline-track-title");
      const trackArtist = entry.trackInfo.querySelector(".offline-track-artist");
      trackTitle.textContent = track.title || "Title unavailable";
      trackTitle.href = window.SkipperFavourites.webSearchUrl(track);
      trackTitle.target = "_blank";
      trackTitle.rel = "noopener";
      trackArtist.textContent = track.artist || "Artist unavailable";
      updateFavouriteButton(entry.favourite, track);
      if (activeMediaPlayer === player) {
        updateSystemMediaMetadata(track, entry);
        renderHeaderHistory(entry, false);
      }
    };

    favourite.addEventListener("click", () => {
      if (!entry.currentTrack) return;
      window.SkipperFavourites.toggle(entry.currentTrack);
      updateFavouriteButton(favourite, entry.currentTrack);
    });
    player.on("timeupdate", renderPlayingTrack);
    player.on("timeupdate", () => {
      if (activeMediaPlayer === player) updateSystemPositionState(player);
    });
    player.on("seeked", renderPlayingTrack);

    button.addEventListener("click", () => {
      if (player.paused()) {
        const playback = player.play();
        if (playback && typeof playback.catch === "function") {
          playback.catch(() => {
            status.textContent = "This programme could not be played.";
          });
        }
      } else {
        stopHistoryPlayer(entry);
      }
    });
    player.on("play", () => {
      enforceExclusivePlayback(player);
      activeMediaPlayer = player;
      activeHeaderEntry = entry;
      renderPlayingTrack();
      renderHeaderHistory(entry, !entry.currentTrack);
      updateSystemMediaMetadata(entry.currentTrack, {
        title: entry.title,
        artist: entry.artist,
        album: entry.album,
        presenter: entry.presenter,
        artwork: entry.artwork,
      });
      updateSystemPlaybackState("playing");
      button.classList.add("is-playing");
      button.setAttribute("aria-label", `Stop ${title}`);
      status.textContent = "Starting…";
    });
    player.on("playing", () => {
      updateSystemPlaybackState("playing");
      updateSystemPositionState(player);
      status.textContent = "Playing now.";
    });
    player.on("pause", () => {
      if (activeMediaPlayer === player) {
        updateSystemPlaybackState("paused");
        updateSystemPositionState(player);
        updateHeaderRadioToggle(false, entry.title);
      }
      button.classList.remove("is-playing");
      button.setAttribute("aria-label", `Play ${title}`);
      if (status.textContent !== "Stopped.") status.textContent = "Paused.";
    });
    player.on("ended", () => stopHistoryPlayer(entry));
    player.on("error", () => {
      if (activeMediaPlayer === player) {
        updateSystemPlaybackState("none");
      }
      button.classList.remove("is-playing");
      button.setAttribute("aria-label", `Play ${title}`);
      status.textContent = "This programme could not be played.";
    });
  });
}

function libraryHistorySignature(history) {
  return JSON.stringify(
    history.map((job) => [
      job.id,
      job.status,
      job.display_title,
      job.description,
      job.album,
      job.artwork_url,
      job.media_url,
      job.file_size,
      job.tracks,
    ]),
  );
}

function renderLibrary(data) {
  const active = Array.isArray(data.active) ? data.active : [];
  const history = Array.isArray(data.history) ? data.history : [];
  activeJobs.replaceChildren(...active.map((job) => createProcessItem(job, true)));
  const nextHistorySignature = libraryHistorySignature(history);
  const historyIsPlaying = Array.from(historyPlayers.values()).some(
    (entry) => !entry.player.paused(),
  );
  if (nextHistorySignature !== historySignature && !historyIsPlaying) {
    disposeHistoryPlayers();
    historyJobs.replaceChildren(...history.map((job) => createProcessItem(job, false)));
    historySignature = nextHistorySignature;
    initialiseHistoryPlayers(history);
  }
  activeCount.textContent = String(active.length);
  historyCount.textContent = String(history.length);
  activeEmpty.hidden = active.length > 0;
  historyEmpty.hidden = history.length > 0;
}

async function loadJobs() {
  if (libraryRequestInFlight) return;
  libraryRequestInFlight = true;
  refreshButton.disabled = true;
  try {
    const response = await fetch("/api/jobs", { cache: "no-store" });
    renderLibrary(await readResponse(response));
    libraryMessage.textContent = "";
  } catch (error) {
    libraryMessage.textContent = `Could not refresh programmes: ${error.message}`;
  } finally {
    libraryRequestInFlight = false;
    refreshButton.disabled = false;
  }
}

function renderJob(job) {
  const progress = Math.max(0, Math.min(100, Number(job.progress) || 0));
  card.hidden = false;
  card.dataset.status = job.status;
  kicker.textContent = job.status === "complete" ? "Complete" : job.status === "failed" ? "Stopped" : "In progress";
  stage.textContent = job.stage_label;
  progressNumber.textContent = `${progress}%`;
  progressBar.style.width = `${progress}%`;
  progressTrack.setAttribute("aria-valuenow", String(progress));
  message.textContent = job.message;
  logs.textContent = (job.logs || []).join("\n");
  logs.scrollTop = logs.scrollHeight;

  const isFinished = job.status === "complete" || job.status === "failed";
  submitButton.disabled = !isFinished;
  input.disabled = !isFinished;

  if (job.status === "complete" && job.download_url) {
    downloadButton.href = job.download_url;
    downloadButton.setAttribute("download", job.filename || "news-skipped-programme");
    downloadButton.hidden = false;
    downloadButton.focus({ preventScroll: true });
  } else {
    downloadButton.hidden = true;
  }
}

async function readResponse(response) {
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(data.error || "Something went wrong. Please try again.");
  }
  return data;
}

async function pollJob(jobId) {
  try {
    const response = await fetch(`/api/jobs/${jobId}`, { cache: "no-store" });
    const job = await readResponse(response);
    renderJob(job);
    loadJobs();
    if (job.status === "complete" || job.status === "failed") {
      clearTimeout(pollTimer);
      return;
    }
    pollTimer = setTimeout(() => pollJob(jobId), 1000);
  } catch (error) {
    message.textContent = error.message;
    pollTimer = setTimeout(() => pollJob(jobId), 2500);
  }
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  clearTimeout(pollTimer);
  submitButton.disabled = true;
  input.disabled = true;
  downloadButton.hidden = true;
  renderJob({
    status: "running",
    stage_label: "Submitting programme",
    progress: 0,
    message: "Sending the BBC Sounds link to Skipper…",
    logs: [],
  });

  try {
    const response = await fetch("/api/jobs", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: input.value.trim() }),
    });
    const job = await readResponse(response);
    displayedJobId = job.id;
    renderJob(job);
    loadJobs();
    pollJob(job.id);
  } catch (error) {
    renderJob({
      status: "failed",
      stage_label: "Check the link",
      progress: 0,
      message: error.message,
      logs: [],
    });
  }
});

refreshButton.addEventListener("click", loadJobs);

historyJobs.addEventListener("click", async (event) => {
  const removeButton = event.target.closest("button[data-job-id]");
  if (!removeButton) return;
  const name = removeButton.dataset.jobName || "this programme";
  if (!window.confirm(`Remove ${name} and its files from the server?`)) return;

  stopHistoryPlayer(historyPlayers.get(removeButton.dataset.jobId));
  removeButton.disabled = true;
  libraryMessage.textContent = `Removing ${name}…`;
  try {
    const jobId = encodeURIComponent(removeButton.dataset.jobId);
    const response = await fetch(`/api/jobs/${jobId}`, { method: "DELETE" });
    await readResponse(response);
    if (displayedJobId === removeButton.dataset.jobId) {
      displayedJobId = undefined;
      card.hidden = true;
      submitButton.disabled = false;
      input.disabled = false;
    }
    libraryMessage.textContent = `${name} was removed from the server.`;
    await loadJobs();
  } catch (error) {
    libraryMessage.textContent = error.message;
    removeButton.disabled = false;
  }
});

nowPlayingFavourite.addEventListener("click", () => {
  const track = activeHeaderEntry?.currentTrack || currentLiveTrack;
  if (!track) return;
  window.SkipperFavourites.toggle(track);
  updateFavouriteButton(nowPlayingFavourite, track);
});

liveNowPlayingFavourite.addEventListener("click", () => {
  if (!currentLiveTrack) return;
  window.SkipperFavourites.toggle(currentLiveTrack);
  updateFavouriteButton(liveNowPlayingFavourite, currentLiveTrack);
});

headerRadioToggle.addEventListener("click", () => {
  const player = activeHeaderEntry?.player || radioPlayer;
  if (!player) return;
  if (player.paused()) {
    const playPromise = player.play();
    if (playPromise?.catch) playPromise.catch(() => updateHeaderRadioToggle(false));
  } else if (activeHeaderEntry) {
    stopHistoryPlayer(activeHeaderEntry);
  } else {
    player.pause();
  }
});

fipToggle?.addEventListener("click", toggleFip);

window.addEventListener("storage", () => {
  updateFavouriteButton(
    nowPlayingFavourite,
    activeHeaderEntry?.currentTrack || currentLiveTrack,
  );
  updateFavouriteButton(liveNowPlayingFavourite, currentLiveTrack);
  historyPlayers.forEach((entry) => {
    updateFavouriteButton(entry.favourite, entry.currentTrack);
  });
});

window.addEventListener("skipper:favourites-changed", () => {
  updateFavouriteButton(
    nowPlayingFavourite,
    activeHeaderEntry?.currentTrack || currentLiveTrack,
  );
  updateFavouriteButton(liveNowPlayingFavourite, currentLiveTrack);
  historyPlayers.forEach((entry) => {
    updateFavouriteButton(entry.favourite, entry.currentTrack);
  });
});

initialiseRadioPlayer();
window.SkipperFavourites.load();
loadNowPlaying();
loadJobs();
setInterval(loadNowPlaying, 5000);
setInterval(alternateHeaderMetadata, 7000);
setInterval(loadJobs, 2000);
