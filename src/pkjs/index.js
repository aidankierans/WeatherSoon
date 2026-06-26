var WEATHER_POLL_MINUTES = 30;

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

function pad2(n) {
  return (n < 10 ? '0' : '') + n;
}

// Find the index in the hourly time array for the current local hour, or the
// first future hour if the current hour isn't present.
function currentHourIndex(times) {
  var now = new Date();
  var prefix = now.getFullYear() + '-' + pad2(now.getMonth() + 1) + '-' +
    pad2(now.getDate()) + 'T' + pad2(now.getHours());
  for (var i = 0; i < times.length; i++) {
    if (times[i].indexOf(prefix) === 0) return i;
  }
  for (var j = 0; j < times.length; j++) {
    if (new Date(times[j]) >= now) return j;
  }
  return 0;
}

function roundOrUnavailable(v) {
  return (v === null || v === undefined) ? -100 : Math.round(v);
}

function locationSuccess(pos) {
  var lat = pos.coords.latitude;
  var lon = pos.coords.longitude;

  var unit = getTempUnit();
  var weatherUrl = 'https://api.open-meteo.com/v1/forecast?' +
    'latitude=' + lat + '&longitude=' + lon +
    '&hourly=temperature_2m,precipitation_probability,uv_index' +
    '&temperature_unit=' + unit +
    '&timezone=auto' +
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
    var idx = currentHourIndex(times);

    var msg = {};
    for (var k = 0; k < 3; k++) {
      var ii = idx + k;
      if (ii < times.length) {
        msg['HOURLY_TEMP_' + k] = roundOrUnavailable(temps[ii]);
        msg['HOURLY_PRECIP_' + k] = roundOrUnavailable(precip[ii]);
        msg['HOURLY_UV_' + k] = roundOrUnavailable(uv[ii]);
      } else {
        msg['HOURLY_TEMP_' + k] = -100;
        msg['HOURLY_PRECIP_' + k] = -100;
        msg['HOURLY_UV_' + k] = -100;
      }
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
