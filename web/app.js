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

let pollTimer;

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
    renderJob(job);
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
