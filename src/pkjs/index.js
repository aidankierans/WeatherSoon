var WEATHER_POLL_MINUTES = 30;
// Hours of forecast sent to the watch, which rolls forward through them on its
// own between fetches. Keep in sync with FORECAST_HOURS in main.c.
var FORECAST_HOURS = 6;

var Clay = require('@rebble/clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

var xhrRequest = function (url, type, callback) {
  var xhr = new XMLHttpRequest();
  xhr.timeout = 15000;
  xhr.onload = function () {
    callback(xhr.status === 200 ? this.responseText : null);
  };
  xhr.onerror = function () { callback(null); };
  xhr.ontimeout = function () { callback(null); };
  xhr.open(type, url);
  xhr.send();
};

function getTempUnit() {
  try {
    var settings = JSON.parse(localStorage.getItem('clay-settings'));
    if (settings && settings.TempUnit) {
      return parseInt(settings.TempUnit, 10) === 1 ? 'celsius' : 'fahrenheit';
    }
  } catch (e) {}
  return 'fahrenheit';
}

// Index of the hourly entry for the current hour: the last one starting at or
// before now. Times are unix seconds.
function currentHourIndex(times, nowSec) {
  var idx = 0;
  for (var i = 0; i < times.length && times[i] <= nowSec; i++) idx = i;
  return idx;
}

function roundOrUnavailable(v) {
  return (v === null || v === undefined) ? -100 : Math.round(v);
}

function valueAt(arr, i) {
  return roundOrUnavailable(i < arr.length ? arr[i] : null);
}

function locationSuccess(pos) {
  var lat = pos.coords.latitude;
  var lon = pos.coords.longitude;

  var unit = getTempUnit();
  var weatherUrl = 'https://api.open-meteo.com/v1/forecast?' +
    'latitude=' + lat + '&longitude=' + lon +
    '&current=temperature_2m,uv_index' +
    '&hourly=temperature_2m,precipitation_probability,uv_index' +
    '&temperature_unit=' + unit +
    '&timezone=auto' +
    '&timeformat=unixtime' +
    '&forecast_days=2';

  xhrRequest(weatherUrl, 'GET', function (weatherResp) {
    if (!weatherResp) {
      console.log('Weather request failed');
      return;
    }

    var data;
    try { data = JSON.parse(weatherResp); } catch (e) {
      console.log('Weather parse error: ' + e);
      return;
    }

    if (!data.hourly || !data.hourly.time) {
      console.log('Weather response missing hourly data');
      return;
    }

    var times = data.hourly.time;
    var temps = data.hourly.temperature_2m || [];
    var precip = data.hourly.precipitation_probability || [];
    var uv = data.hourly.uv_index || [];
    var idx = currentHourIndex(times, Date.now() / 1000);

    // Current conditions come from 15-minutely model data, so they track the
    // "NOW" slot better than the top-of-the-hour forecast value.
    var current = data.current || {};

    var msg = {
      WEATHER_BASE_TIME: times[idx],
      CURRENT_TIME: current.time || 0,
      CURRENT_TEMP: roundOrUnavailable(current.temperature_2m),
      CURRENT_UV: roundOrUnavailable(current.uv_index)
    };
    for (var k = 0; k < FORECAST_HOURS; k++) {
      msg['HOURLY_TEMP_' + k] = valueAt(temps, idx + k);
      // precipitation_probability covers the preceding hour, so the chance
      // for the hour starting at idx + k is reported at idx + k + 1.
      msg['HOURLY_PRECIP_' + k] = valueAt(precip, idx + k + 1);
      msg['HOURLY_UV_' + k] = valueAt(uv, idx + k);
    }

    Pebble.sendAppMessage(msg,
      function (e) { console.log('Weather sent successfully'); },
      function (e) { console.log('Error sending weather: ' + JSON.stringify(e)); }
    );
  });
}

function locationError(err) {
  console.log('Error requesting location: ' + err);
}

function getWeather() {
  navigator.geolocation.getCurrentPosition(
    locationSuccess,
    locationError,
    { timeout: 15000, maximumAge: WEATHER_POLL_MINUTES * 60 * 1000 }
  );
}

Pebble.addEventListener('ready', function (e) {
  console.log('PebbleKit JS ready');
  getWeather();
});

Pebble.addEventListener('appmessage', function (e) {
  console.log('AppMessage received');
  getWeather();
});
