# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Run the example's exact production gzip bundle, including startup, in Node."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest

from ui_sources import ROOT, bundler

EXAMPLE = ROOT / "examples/external-web/ui/product.json"
HARNESS = Path(__file__).with_name("ui_composition_harness.cjs")


@pytest.fixture(scope="module")
def composition():
    return bundler.load_composition(EXAMPLE)


@pytest.fixture(scope="module")
def bundle(composition, tmp_path_factory):
    assets, _ = bundler.build(composition, 163840)
    path = tmp_path_factory.mktemp("external-ui") / "app.js"
    path.write_bytes(next(raw for url, _, raw, _ in assets if url == "/app.js"))
    return path


def run_ui(bundle, tmp_path, scenario, **options):
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is not installed")
    script = tmp_path / "scenario.js"
    script.write_text(scenario)
    result = subprocess.run([node, str(HARNESS), str(bundle), str(script), json.dumps(options)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_production_bundle_navigation_branding_storage_and_optional_assets(bundle, composition, tmp_path):
    assert {asset["url"] for asset in composition["assets"]} == {"/index.html", "/app.js", "/app.css", "/favicon.svg"}
    assert all(module["source"].startswith(("shared:", "generated:")) or module["source"] == "product:status.js"
               for module in composition["modules"])
    assert "station" not in bundle.read_text().lower()
    run_ui(bundle, tmp_path, r"""
      assert(shell.started && live.running);
      assert.equal(shell.current.name, '');
      assert.equal(env.text('brand-name'), 'Counter');
      assert.equal(doc.title, 'Online · Counter');
      assert.deepEqual(JSON.parse(JSON.stringify(UI.navigation)), [['', 'Status'], ['settings', 'Settings']]);
      assert.equal(viewOf('receiver'), undefined);
      assert.equal(viewOf('ntrip'), undefined);
      assert.equal(viewOf('map'), undefined);
      assert.equal(storageKey('theme'), 'counter.theme');
      assert.equal(store.get('theme', 'auto'), 'auto');
      setTheme('light');
      assert.equal(JSON.parse(env.storageMap.get('counter.theme')), 'light');
      assert.equal(env.storageMap.get('station.theme'), 'dark');
      sessionStorage.setItem('station.restartedAt', String(Date.now()));
      assert.equal(restartedRecently(), false);
      sessionStorage.setItem(storageKey('restartedAt'), String(Date.now()));
      assert.equal(restartedRecently(), true);
      assert.equal(restartNote(), 'The device restarted. Login again.');
      assert.equal(sessionStorage.getItem('station.restartedAt'), String(Date.now()));
      location.hash = '#/settings'; route(); await env.flush();
      assert.equal(shell.current.name, 'settings');
      assert(env.text('tabs').includes('Settings'));
      assert(env.text('view-body').includes('Status visibility'));
      assert(env.sent.every(r => !/caster|receiver|health|onocoy|report/.test(r.path)));
    """, storage={"station.theme": "dark"})


def test_second_section_replacement_keyed_extension_and_reconnect_full_resync(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      applyStatusMessage({ full: true, uptime_s: 20, data: { product: {counter: 8}, network: {online: true, ssid: 'known'} } });
      applyStatusMessage({ full: false, uptime_s: 21, data: { network: {online: false} } });
      assert.equal(S.status.product.counter, 8);
      assert.equal(S.status.network.online, false);
      assert.equal(S.status.network.ssid, undefined); // whole section replacement
      UI.mergeSections.channels = (old, delta) => {
        const byId = new Map((old || []).map(item => [item.id, item]));
        for (const item of delta) byId.set(item.id, item);
        return [...byId.values()];
      };
      applyStatusMessage({ data: { channels: [{id:'a', value:1}, {id:'b', value:2}] } });
      applyStatusMessage({ data: { channels: [{id:'a', value:3}] } });
      assert.equal(S.status.channels.length, 2);
      assert.equal(S.status.channels[0].value, 3);
      live.handle({ type:'hello', api:1, admin:true });
      live.handle({ type:'status', full:true, uptime_s:30, data:{ product:{counter:9}, channels:[{id:'b',value:4}] } });
      assert.equal(live.state, 'ws');
      assert.equal(S.status.channels.length, 1); // removal/full bypasses merger
      assert.equal(S.status.network, undefined); // no stale section survives full resync
      assert.equal(S.statusAt, Date.now());
      assert.equal(classifyStatus(S.status, true).key, 'unknown');
      live.stop(); live.start();
      assert.equal(live.state, 'connecting');
      live.handle({type:'status', full:true, uptime_s:31, data:{network:{online:true}}});
      assert.equal(classifyStatus(S.status, true).key, 'online');
    """)


def test_unknown_and_stale_fields_never_invent_receiver_state(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      applyStatusMessage({full:true, data:{product:{counter:null}, network:{online:null}}});
      assert.equal(classifyStatus(S.status, true).key, 'unknown');
      assert(env.text('view-body').includes('Unknown'));
      assert(env.text('view-body').includes(DASH));
      env.now += 7000;
      renderLive(); shell.current.view.update(S.status);
      assert(doc.body.classList.contains('no-contact'));
      assert.equal(env.ids.live.querySelector('a').getAttribute('aria-label'), 'No contact with the device');
      assert(env.text('view-body').includes('Last known values: no recent contact.'));
      assert(!/receiver|caster|health/.test(JSON.stringify(S.status)));
      applyStatusMessage({full:true,data:{network:{online:true},product:{counter:10}}});
      assert(!doc.body.classList.contains('no-contact'));
      assert(!env.text('view-body').includes('Last known'));
    """)


def test_product_steps_claim_without_caster_credentials_and_receiver_absent_recovery(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      assert.equal(UI.commissioning, null);
      assert.deepEqual(Array.from(UI.commissioningSteps, x=>x.key), ['network','visibility']);
      assert(env.text('view-body').includes('Set an admin password'));
      assert.deepEqual(Array.from(env.sent, x=>x.path), ['/api/session']); // no admin reads before claim
      const form = env.ids['view-body'].querySelector('form');
      for (const input of form.querySelectorAll('input')) input.value = 'valid-password';
      form.fire('submit'); await env.flush();
      assert(env.text('view-body').includes('Check access'));
      assert(env.text('view-body').includes('No receiver is required.'));
      assert(env.text('view-body').includes('hold BOOT for 10 seconds'));
      env.ids['view-body'].querySelector('button').fire('click'); await env.flush();
      assert(env.text('view-body').includes('Choose status visibility'));
      env.ids['view-body'].querySelector('button').fire('click'); await env.flush();
      assert(shell.started && shell.current.name === '');
      assert.equal(env.preferences.setup_done, true);
      assert(env.sent.every(r => ['/api/session','/api/setup/admin','/api/product/settings','/api/status'].includes(r.path)));
    """, session={"authenticated": False, "admin_set": False, "setup_done": False})


def test_commissioning_save_failure_keeps_inputs_and_resume_is_product_owned(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      assert(env.text('view-body').includes('Check access'));
      env.failSave = true;
      const before = env.ids['view-body'].querySelector('button');
      before.fire('click'); await env.flush();
      assert.strictEqual(env.ids['view-body'].querySelector('button'), before);
      assert(env.text('view-body').includes('Check access'));
      env.failSave = false;
      before.fire('click'); await env.flush();
      const checkbox = env.ids['view-body'].querySelector('input'); checkbox.checked = true;
      env.failSave = true;
      env.ids['view-body'].querySelector('button').fire('click'); await env.flush();
      assert.strictEqual(env.ids['view-body'].querySelector('input'), checkbox);
      assert.equal(checkbox.checked, true);
      env.failSave = false;
      await startCommissioning(false);
      assert(env.text('view-body').includes('Choose status visibility'));
    """, session={"setup_done": False})


def test_commissioning_step_that_fails_to_open_offers_a_retry(bundle, tmp_path):
    for failure in ("throw new Error('no network');", "return Promise.reject(new Error('no network'));"):
        run_ui(bundle, tmp_path, r"""
          const original = UI.commissioningSteps[0];
          let fail = true;
          UI.commissioningSteps[0] = { key: original.key, title: original.title,
            create(ctx) { if (fail) { FAILURE } return original.create(ctx); } };
          await startCommissioning(false); await env.flush();
          assert(env.text('view-body').includes('Try again'));
          fail = false;
          env.ids['view-body'].querySelector('button').fire('click'); await env.flush();
          assert(env.text('view-body').includes('Check access'));
        """.replace("FAILURE", failure), session={"setup_done": False})


def test_an_edit_during_a_save_stays_dirty(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      location.hash = '#/settings'; route(); await env.flush();
      const page = shell.current, input = env.ids['view-body'].querySelector('input');
      input.checked = true; input.fire('input', true);
      env.ids['view-body'].querySelector('form').fire('submit');
      input.checked = false; input.fire('input', true);
      await env.flush();
      assert(page.view.dirty());
      assert.strictEqual(env.ids['view-body'].querySelector('input'), input);
      assert.equal(input.checked, false);
      env.ids['view-body'].querySelector('form').fire('submit'); await env.flush();
      assert(!page.view.dirty());
    """)


def test_a_failed_reload_leaves_an_edited_form_alone(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      location.hash = '#/settings'; route(); await env.flush();
      const page = shell.current, input = env.ids['view-body'].querySelector('input');
      input.checked = true; input.fire('input', true);
      env.failLoad = true;
      await page.view.reload(); await env.flush();
      env.failLoad = false;
      assert.strictEqual(env.ids['view-body'].querySelector('input'), input);
      assert.equal(input.checked, true);
      assert(page.view.dirty());
    """)


def test_dirty_forms_multitab_attribution_and_navigation_guard(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      location.hash = '#/settings'; route(); await env.flush();
      const page = shell.current, input = env.ids['view-body'].querySelector('input');
      input.checked = true; input.fire('input', true);
      assert(page.view.dirty());
      bus.emit('event', {event:'settings_changed',client:CLIENT_ID}); env.settle();
      assert.equal(env.toasts.length, 0);
      bus.emit('event', {event:'settings_changed',client:'another-tab'});
      bus.emit('event', {event:'settings_changed',client:'another-tab'}); env.settle();
      assert.equal(env.toasts.length, 1);
      assert.equal(env.toasts[0].text, 'Settings were changed elsewhere. Reload this page to see them.');
      assert.strictEqual(env.ids['view-body'].querySelector('input'), input);
      assert.equal(input.checked, true);
      location.hash = '#/'; route();
      assert.strictEqual(shell.current, page);
      assert.equal(location.hash, '#/settings');
      env.ids['view-body'].querySelector('form').fire('submit'); await env.flush();
      assert(!page.view.dirty());
      assert.equal(env.sent.at(-1).headers['X-Client-Id'], CLIENT_ID);
      bus.emit('event', {event:'settings_changed',client:'another-tab'}); env.settle(); await env.flush();
      assert.notStrictEqual(env.ids['view-body'].querySelector('input'), input);
      location.hash = '#/'; route();
      assert.equal(shell.current.name, '');
    """)


def test_recovery_without_settings_or_receiver_does_not_trap_user_in_setup(bundle, tmp_path):
    run_ui(bundle, tmp_path, r"""
      assert(shell.started && shell.current.name === '');
      assert(!env.sent.some(r => r.path === '/api/product/settings'));
      assert(!env.sent.some(r => /receiver|caster/.test(r.path)));
      const steps = recoverySteps().textContent;
      assert(steps.includes('BOOT button on the device for 10 seconds'));
    """, session={"setup_done": False, "settings_available": False})


def test_product_noun_in_all_shared_reachable_strings_and_capitalized_form(bundle, composition, tmp_path):
    # The production bundle strips comments. Its entire string/code surface
    # contains no station token, covering branches beyond the rendered scenes.
    assert not re.search(r"station", bundle.read_text(), re.I)
    run_ui(bundle, tmp_path, r"""
      assert.equal(productText('{Product}: {product}'), 'Device: device');
      S.status = null; S.statusAt = 0; renderLive();
      assert.equal(env.ids.live.querySelector('.in').getAttribute('title'), 'Connecting to the device');
      assert(!/station/i.test(recoverySteps().textContent));
      showNoAdmin(); assert(!/station/i.test(env.text('view-body')));
      assert(!/station/i.test(errText(new ApiError(403, {code:'not_on_setup_ap',
        message:"The first password can only be set over the station's own Wi-Fi"}))));
      for (const result of ['no_response','http_error','untrusted_redirect','bad_manifest']) {
        assert(!/station/i.test(checkFailure({last_check_result:result}).text));
      }
      UI.metadata.product_noun = '$&unit';
      assert.equal(productText('The {product}'), 'The $&unit'); // literal replacement, no replacement-string expansion
    """)


def test_canonical_theme_and_shell_assets_are_reused_byte_for_byte(composition):
    assets, _ = bundler.build(composition, 163840)
    by_url = {url: raw for url, _, raw, _ in assets}
    assert by_url['/app.css'] == (ROOT / 'components/web_shell/ui/app.css').read_bytes()
    assert by_url['/favicon.svg'] == (ROOT / 'components/web_shell/ui/favicon.svg').read_bytes()
    assert by_url['/index.html'].decode() == bundler.render_index(
        (ROOT / 'components/web_shell/ui/index.html').read_text(), composition)


def test_external_component_closure_rejects_forbidden_edges_even_without_linking(tmp_path):
    spec = importlib.util.spec_from_file_location('check_external_web', ROOT / 'tools/ci/check_external_web.py')
    check = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(check)
    info = {name: {'dir': str(ROOT / 'components' / name), 'reqs': []} for name in check.SUPPORTED}
    assert set(check.validate({'build_component_info': info}, ROOT)) == check.SUPPORTED
    for forbidden in check.FORBIDDEN:
        info['web_shell']['priv_reqs'] = [forbidden]
        with pytest.raises(ValueError, match='forbidden'):
            check.validate({'build_component_info': info}, ROOT)
    info['web_shell']['priv_reqs'] = []
    del info['ledger']
    with pytest.raises(ValueError, match='closure'):
        check.validate({'build_component_info': info}, ROOT)


def test_external_web_ci_is_a_release_prerequisite_with_runtime_checks():
    from test_ci_job import ci
    workflow = ci.load_workflow((ROOT / '.github/workflows/build.yml').read_text())
    assert 'external-web' in workflow['jobs']['release']['needs']
    plan = ci.plan(workflow, 'external-web')
    assert plan
    steps = workflow['jobs']['external-web']['steps']
    commands = '\n'.join(step.get('run', '') for step in steps)
    assert 'tools/ci/external_web.sh /tmp/onodiy-external-web --qemu' in commands
    assert 'tools/qemu/external_web.py' in commands
    assert '--build-dir /tmp/onodiy-external-web/build-16MB-qemu' in commands
