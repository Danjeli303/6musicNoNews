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

let pollTimer;
let displayedJobId;
let libraryRequestInFlight = false;
let radioPlayer;
let historySignature = "";
const historyPlayers = new Map();

function stopHistoryPlayer(entry) {
  if (!entry) return;
  entry.player.pause();
  try {
    entry.player.currentTime(0);
  } catch (_error) {
    // The player may not have loaded its metadata yet.
  }
  entry.button.classList.remove("is-playing");
  entry.button.textContent = "▶";
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
    radioStatus.textContent = "Connecting to the live stream…";
  });
  radioPlayer.on("playing", () => {
    radioStatus.textContent = "Playing live · News-skipped stream";
  });
  radioPlayer.on("waiting", () => {
    radioStatus.textContent = "Reconnecting to the live stream…";
  });
  radioPlayer.on("pause", () => {
    radioStatus.textContent = "Live stream paused.";
  });
  radioPlayer.on("error", () => {
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
      const playButton = makeElement("button", "artwork-play", "▶");
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
      playerWrap.append(audio, playerStatus);
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
    if (!button || !status || !document.getElementById(audioId)) return;

    const title = job.display_title || job.title || job.filename || "programme";
    const player = window.videojs(audioId, {
      audioOnlyMode: true,
      autoplay: false,
      controls: true,
      preload: "none",
      responsive: true,
    });
    const entry = { player, button, status, title };
    historyPlayers.set(job.id, entry);

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
      button.classList.add("is-playing");
      button.textContent = "■";
      button.setAttribute("aria-label", `Stop ${title}`);
      status.textContent = "Starting…";
    });
    player.on("playing", () => {
      status.textContent = "Playing now.";
    });
    player.on("pause", () => {
      button.classList.remove("is-playing");
      button.textContent = "▶";
      button.setAttribute("aria-label", `Play ${title}`);
      if (status.textContent !== "Stopped.") status.textContent = "Paused.";
    });
    player.on("ended", () => stopHistoryPlayer(entry));
    player.on("error", () => {
      button.classList.remove("is-playing");
      button.textContent = "▶";
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
    libraryMessage.textContent = `Could not refresh activity: ${error.message}`;
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

initialiseRadioPlayer();
loadJobs();
setInterval(loadJobs, 2000);
