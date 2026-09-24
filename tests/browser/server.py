#!/usr/bin/env python3
"""Synthetic UI fixture server. Loopback only, never compiled into firmware.

This tests browser behavior, not ESP32 authorization/security implementation.
It never contacts Tesla or a device and uses no real provisioning data.
"""
import argparse
import copy
import hashlib
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / 'main'
SETTINGS = dict(vin='5YJ3E1EA7KF000001', home_lat=0, home_lon=0, region='NA', enable_m=100,
    disable_m=200, max_age_s=120, future_s=30, lease_s=900, sleep_s=86400, poll_s=600,
    dwell_s=30, daily_cap=400, monthly_cap=12000, dry_run=True)
BASE = dict(mode='DISABLED', commissioned=False, dry_run=True, auto_home=False, desired_on=False,
    gpio_command='OFF commanded', reason='uncommissioned', lease_s=0, override_s=0, vehicle='online',
    location_age_s=-1, distance_m=-1, reported_distance_m=2.3, last_success_uptime_s=0, next_poll_s=-1,
    wifi_connected=True, rssi_dbm=-48, uptime_s=600, utc_ready=True, error='malformed_data',
    reauthorization_needed=False, poll_busy=False, fleet_endpoint='location', fleet_http_status=200,
    fleet_detail='gps_as_of_out_of_range', gps_source_value=-123456789, gps_source_text='-123456789',
    fleet_txid='synthetic-request-id', fleet_date='Wed, 23 Sep 2026 12:00:00 GMT', fleet_received_utc_s=1790164800,
    report_timestamp_text='1790164800000', api_version=95, attempts_this_boot=[1,1,1],
    reserved_today=[4,4,4], reserved_month=[8,8,8], location_monthly_cap=5000,
    estimated_location_usd=.016, ready=True,
    fault=False, fault_source='none', control_max_gap_ms=53, internal_heap_free=79000,
    firmware_version='synthetic-test', sdk_version='v5.5.2', session_left_s=900, settings=SETTINGS)
HISTORY = [dict(uptime_s=500, reason='uncommissioned', commanded_on=False)]
SALT = bytes.fromhex('00112233445566778899aabbccddeeff')
MATERIAL = hashlib.pbkdf2_hmac('sha256', b'synthetic-admin-password', SALT, 100000).hex()

class Fixture:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()
    def reset(self):
        self.state = copy.deepcopy(BASE)
        self.history = copy.deepcopy(HISTORY)
        self.auth = False
        self.requests = []
        self.status_delay = 0
        self.action_delay = 0
        self.events_fail = False
        self.login_type = None

FIXTURE = Fixture()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def send(self, obj, status=200, content_type='application/json', extra=None):
        data = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Security-Policy', "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        self.send_header('X-Content-Type-Options', 'nosniff')
        for k,v in (extra or {}).items():
            self.send_header(k,v)
        self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError): pass
    def do_GET(self):
        if self.path in ['/', '/style.css', '/app.js']:
            name = 'index.html' if self.path == '/' else self.path[1:]
            kind = {'index.html':'text/html', 'style.css':'text/css', 'app.js':'text/javascript'}[name]
            return self.send((ROOT/name).read_bytes(), content_type=kind)
        with FIXTURE.lock:
            if self.path == '/__test':
                return self.send(dict(state=FIXTURE.state, requests=FIXTURE.requests, login_type=FIXTURE.login_type))
            if self.path == '/api/login-info':
                return self.send(dict(version=2, salt=SALT.hex(), iterations=100000))
            FIXTURE.requests.append(self.path)
            if not FIXTURE.auth: return self.send(dict(error='authentication_required'), 401)
            if self.path == '/api/status':
                response = copy.deepcopy(FIXTURE.state);delay=FIXTURE.status_delay
            elif self.path == '/api/events':
                if FIXTURE.events_fail:return self.send(dict(error='status_unavailable'),503)
                return self.send(dict(events=FIXTURE.history))
            else:return self.send(dict(error='not_found'),404)
        time.sleep(delay)
        self.send(response, extra={'X-CSRF-Token':'synthetic-csrf'})
    def do_POST(self):
        size=int(self.headers.get('Content-Length','0'))
        if size>8192:return self.send(dict(error='invalid_request'),400)
        body=json.loads(self.rfile.read(size))
        with FIXTURE.lock:
            if self.path == '/__test':
                if body.pop('reset',False):FIXTURE.reset()
                FIXTURE.state.update(body.pop('state',{}))
                for key in ['auth','status_delay','action_delay','events_fail']:
                    if key in body:setattr(FIXTURE,key,body[key])
                return self.send(dict(ok=True))
            if self.path == '/api/login':
                FIXTURE.login_type = 'material' if 'material' in body else 'password'
                if body.get('material')!=MATERIAL:return self.send(dict(error='login_failed'),401)
                FIXTURE.auth=True
                return self.send(dict(ok=True))
            action=body.get('action');FIXTURE.requests.append(action)
            if not FIXTURE.auth or self.headers.get('X-CSRF-Token')!='synthetic-csrf':
                return self.send(dict(error='authentication_or_csrf'),403)
            s=FIXTURE.state
            if action=='logout':FIXTURE.auth=False
            elif action=='off':s.update(mode='DISABLED',gpio_command='OFF commanded',auto_home=False,lease_s=0,override_s=0,reason='user_disabled')
            elif s['fault']:return self.send(dict(error='usb_recovery_required'),409)
            elif action=='auto':s.update(mode='AUTO',reason='no_auto_authorization',next_poll_s=600)
            elif action=='timed_on':
                if not s['commissioned'] or s['mode']=='DISABLED':return self.send(dict(error='timed_on_rejected'),409)
                s.update(mode='TIMED_ON',override_s=body['seconds'],reason='timed_override_bypasses_presence')
            elif action=='check_now':
                if s['mode']=='DISABLED':return self.send(dict(error='check_requires_auto'),409)
                s['poll_busy']=True
            elif action=='settings':
                if s['dry_run'] and not body['settings']['dry_run']:return self.send(dict(error='usb_commissioning_required'),409)
                s.update(settings=body['settings'],dry_run=body['settings']['dry_run'],auto_home=False,lease_s=0,override_s=0)
            else:return self.send(dict(error='unknown_action'),400)
            delay=0 if action=='off' else FIXTURE.action_delay
        time.sleep(delay)
        self.send(dict(ok=True))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',type=int,default=0);args=p.parse_args()
    server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
    print(server.server_address[1],flush=True)
    try:server.serve_forever()
    finally:server.server_close()
