import test from 'node:test';
import assert from 'node:assert/strict';
import {isJournalKey, journalKey} from './settings.mjs';

test('journal keys carry the v1 prefix and the scope', () => {
  assert.equal(journalKey('regular', 7), 'handoff.v1.regular.7');
  assert.equal(journalKey('private', 9), 'handoff.v1.private.9');
});

test('the popup recognises both scopes and rejects the retired prefix', () => {
  assert.equal(isJournalKey('handoff.v1.regular.7'), true);
  assert.equal(isJournalKey('handoff.v1.private.9'), true);
  // The popup filtered this prefix while the adapters wrote the one above, so
  // "recent takeovers" was always empty.
  assert.equal(isJournalKey('handoff.journal.7'), false);
  assert.equal(isJournalKey('handoff.settings.v1'), false);
});
