'use strict';
// Estimate a turn's token usage from the transcript the harness already wrote,
// for routes whose provider reports no usage at all.
//
// WHY THIS EXISTS. chocobar counts tokens from what each harness records. A
// route that is a real API echoes a usage block, so those numbers are exact.
// A route that is a local app or CLI driven through a bridge (CodeBuddy over
// workbuddy, the MiMo desktop, the agy CLI) has no API to answer, so its turns
// arrive with an all-zero usage object and are dropped entirely.
//
// THE MODEL, both facts measured against 63,986 turns from one real pi store
// that DID report usage:
//   1. The prompt is roughly the whole transcript, at about 4 characters per
//      token, scaled by inputFactor.
//   2. The harness compacts long conversations, so the prompt saturates near
//      saturateTokens and stops growing however long the transcript gets. That
//      cap is the important part: the raw character count of a long session is
//      tens of millions of tokens while the prompt actually sent stays around
//      160k. Without the cap the estimate runs away and reports single turns
//      of millions of tokens, which no model can accept.
// The completion side needs no such correction - it is the reply's own text,
// and characters-over-four matches the reported output to within about 2%.
//
// ACCURACY. Summed over turns, the prompt estimate is unbiased: median
// estimated/reported = 1.00 across the calibration set. A SINGLE turn is not
// trustworthy, because compaction is a step - an individual turn is either
// close or several times off, and the stored record does not say which. So
// this is fit for per-model and per-app totals, which is what every consumer
// of aggregate() shows, and unfit for per-session or per-day figures.
//
// The constants are tuned to one machine's history and are config-overridable
// (tokens.estimate); the whole feature is off by default.

const CHARS_PER_TOKEN = 4;

const DEFAULTS = {
  inputFactor: 0.84,       // chars -> prompt tokens, before saturation
  saturateTokens: 185000,  // the prompt stops growing here once compaction starts
  outputFactor: 1.019      // chars -> completion tokens
};

// transcriptChars: characters of text in the whole transcript up to and
//                  including this turn (system + history + tools).
// outputChars:     characters of this turn's own reply.
// Returns null when there is nothing to go on, so the caller can skip the turn
// instead of recording a fabricated zero.
function estimateUsage(transcriptChars, outputChars, cfg) {
  const c = Object.assign({}, DEFAULTS, cfg || {});
  const t = Math.max(0, Number(transcriptChars) || 0);
  const o = Math.max(0, Number(outputChars) || 0);
  if (t <= 0 && o <= 0) return null;
  const prompt = Math.min(t / CHARS_PER_TOKEN * c.inputFactor, c.saturateTokens);
  const output = o / CHARS_PER_TOKEN * c.outputFactor;
  // The cache split is unknowable from a transcript, so the whole prompt is
  // reported as raw input. Every consumer sums input + output + cacheRead +
  // cacheWrite, and that total is what the two terms above model.
  return {
    input: Math.round(prompt),
    output: Math.round(output),
    cacheRead: 0,
    cacheWrite: 0
  };
}

module.exports = { estimateUsage, DEFAULTS, CHARS_PER_TOKEN };
