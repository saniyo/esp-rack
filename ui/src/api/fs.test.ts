// settleWrite(): how a queued file manager write is waited for.
import { afterEach, beforeEach, describe, expect, it, jest } from '@jest/globals';

import { AXIOS } from './endpoints';
import { settleWrite, WRITE_ABANDONED, WRITE_SLOW_MS, WRITE_STATE_UNKNOWN } from './fs';

const queued = (op = 7): any => Promise.resolve({ status: 202, data: { ok: true, status: 'queued', op } });
const status = (state: string, extra: object = {}): any =>
  Promise.resolve({ data: { op: 7, kind: 'delete', state, steps: 3, path: '/x', ...extra } });
const httpError = (code: number): any => Promise.reject(Object.assign(new Error(`http ${code}`), { response: { status: code } }));

// Moves the (fake) clock on, then lets the awaits that follow run.
async function tick(ms: number) {
  jest.advanceTimersByTime(ms);
  for (let i = 0; i < 10; ++i) {
    // eslint-disable-next-line no-await-in-loop
    await Promise.resolve();
  }
}

// Runs the timers until the promise settles.
async function settle<T>(p: Promise<T>): Promise<{ ok: boolean; v?: T; e?: any }> {
  let out: { ok: boolean; v?: T; e?: any } | null = null;
  p.then((v) => { out = { ok: true, v }; }, (e) => { out = { ok: false, e }; });
  for (let i = 0; i < 1000 && !out; ++i) {
    // eslint-disable-next-line no-await-in-loop
    await tick(500);
  }
  return out || { ok: false, e: new Error('never settled') };
}

describe('settleWrite', () => {
  let get: any;
  beforeEach(() => {
    jest.useFakeTimers();
    get = jest.spyOn(AXIOS, 'get');
  });
  afterEach(() => {
    get.mockRestore();
    jest.useRealTimers();
  });

  it('a 200 is done at once, without polls', async () => {
    const r = await settle(settleWrite(Promise.resolve({ status: 200, data: { ok: true } }) as any));
    expect(r.ok).toBe(true);
    expect(get).not.toHaveBeenCalled();
  });

  it('polls a queued one until done, asking again on 503', async () => {
    get.mockImplementationOnce(() => httpError(503)).mockImplementationOnce(() => status('running'))
      .mockImplementationOnce(() => status('done'));
    const onQueued = jest.fn();
    const r = await settle(settleWrite(queued(), { onQueued }));
    expect(r.ok).toBe(true);
    expect(onQueued).toHaveBeenCalledTimes(1);
    expect(get).toHaveBeenCalledTimes(3);
  });

  it("rejects with the device's error", async () => {
    get.mockImplementation(() => status('error', { error: 'io_error' }));
    const r = await settle(settleWrite(queued()));
    expect(r.ok).toBe(false);
    expect(r.e.message).toBe('io_error');
  });

  it('a 404 is not success: the state is unknown', async () => {
    get.mockImplementation(() => httpError(404));
    const r = await settle(settleWrite(queued()));
    expect(r.ok).toBe(false);
    expect(r.e.message).toBe(WRITE_STATE_UNKNOWN);
  });

  it('a long one says so once and keeps polling', async () => {
    const t0 = Date.now();
    get.mockImplementation(() => (Date.now() - t0 > WRITE_SLOW_MS + 10000 ? status('done') : status('running')));
    const onSlow = jest.fn();
    const r = await settle(settleWrite(queued(), { onSlow }));
    expect(r.ok).toBe(true);
    expect(onSlow).toHaveBeenCalledTimes(1);
  });

  it('stops when the page goes', async () => {
    get.mockImplementation(() => status('running'));
    const c = new AbortController();
    const p = settleWrite(queued(), { signal: c.signal });
    for (let i = 0; i < 4; ++i) {
      // eslint-disable-next-line no-await-in-loop
      await tick(500);
    }
    expect(get.mock.calls.length).toBeGreaterThan(0);
    const calls = get.mock.calls.length;
    c.abort();
    const r = await settle(p);
    expect(r.ok).toBe(false);
    expect(r.e.message).toBe(WRITE_ABANDONED);
    expect(get.mock.calls.length).toBe(calls);
  });
});
