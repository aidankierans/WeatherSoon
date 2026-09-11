var WEATHER_POLL_MINUTES = 30;
// Hours of forecast sent to the watch, which rolls forward through them on its
// own between fetches. Keep in sync with FORECAST_HOURS in main.c.
var FORECAST_HOURS = 6;

var FORECAST_URL = 'https://api.open-meteo.com/v1/forecast';
var AIR_QUALITY_URL = 'https://air-quality-api.open-meteo.com/v1/air-quality';

// Regional temperature models that beat best_match, keyed by the timezone
// Open-Meteo reported on the previous fetch. Timezones work as a country
// check; a bounding box around France and Spain would spill into Belgium,
// Germany, Switzerland, Italy, and Portugal.
var TIMEZONE_MODELS = {
  'Europe/Paris': 'meteofrance_seamless',
  'Europe/Madrid': 'meteofrance_seamless',
  'Europe/Andorra': 'meteofrance_seamless',
  'Europe/Monaco': 'meteofrance_seamless'
};

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

function parseJson(text) {
  if (!text) return null;
  try { return JSON.parse(text); } catch (e) { return null; }
}

function getTempUnit() {
  try {
    var settings = JSON.parse(localStorage.getItem('clay-settings'));
    if (settings && settings.TempUnit) {
      return parseInt(settings.TempUnit, 10) === 1 ? 'celsius' : 'fahrenheit';
    }
  } catch (e) {}
  return 'fahrenheit';
}

function lastTimezone() {
  try { return localStorage.getItem('lastTimezone'); } catch (e) { return null; }
}

function saveTimezone(tz) {
  try { if (tz) localStorage.setItem('lastTimezone', tz); } catch (e) {}
}

// Regional temperature models to try, most preferred first; best_match is
// always requested alongside as the fallback. NOAA's National Blend of Models
// covers the lower 48, southern Canada, and northern Mexico; Canada's GEM
// covers the rest of Canada.
function regionalCandidates(lat, lon) {
  var tzModel = TIMEZONE_MODELS[lastTimezone()];
  if (tzModel) return [tzModel];
  if (lat >= 20 && lat <= 84 && lon >= -141 && lon <= -52.5) {
    return lat >= 49 ? ['ncep_nbm_conus', 'gem_seamless'] : ['ncep_nbm_conus'];
  }
  return [];
}

// A timezone-chosen model only applies while the location is still in one of
// its timezones (the user may have travelled since the last fetch).
function modelFitsTimezone(model, tz) {
  for (var key in TIMEZONE_MODELS) {
    if (TIMEZONE_MODELS[key] === model) return TIMEZONE_MODELS[tz] === model;
  }
  return true;
}

// Fetch the forecast with the first candidate model the API can serve. Outside
// a model's grid Open-Meteo answers with unparseable JSON ("latitude": nan),
// which falls through to the next candidate and finally best_match alone.
// Calls back with (data, model), where model is null for best_match alone.
function fetchForecast(lat, lon, candidates, callback) {
  var model = candidates.length ? candidates[0] : null;
  // The current block reflects only the first model listed, so the regional
  // model goes first.
  var url = FORECAST_URL +
    '?latitude=' + lat + '&longitude=' + lon +
    '&models=' + (model ? model + ',' : '') + 'best_match' +
    '&current=temperature_2m' +
    '&hourly=temperature_2m,precipitation_probability,uv_index' +
    '&temperature_unit=' + getTempUnit() +
    '&timezone=auto' +
    '&timeformat=unixtime' +
    '&forecast_days=2';

  xhrRequest(url, 'GET', function (resp) {
    var data = parseJson(resp);
    if (data && data.hourly && data.hourly.time) {
      callback(data, model);
    } else if (resp && model) {
      console.log('Forecast unavailable from ' + model + ', trying the next model');
      fetchForecast(lat, lon, candidates.slice(1), callback);
    } else {
      callback(null, null);  // network failure, or best_match itself failed
    }
  });
}

// Copernicus (CAMS) UV forecast. It peaks at solar noon and tracks official UV
// forecasts more closely than the GFS-derived uv_index in the forecast API.
function fetchUv(lat, lon, callback) {
  var url = AIR_QUALITY_URL +
    '?latitude=' + lat + '&longitude=' + lon +
    '&hourly=uv_index' +
    '&timezone=auto' +
    '&timeformat=unixtime' +
    '&forecast_days=2';

  xhrRequest(url, 'GET', function (resp) {
    var data = parseJson(resp);
    callback(data && data.hourly && data.hourly.time ? data.hourly : null);
  });
}

// Index of the hourly entry for the current hour: the last one starting at or
// before now. Times are unix seconds.
function currentHourIndex(times, nowSec) {
  var idx = 0;
  for (var i = 0; i < times.length && times[i] <= nowSec; i++) idx = i;
  return idx;
}

// Multi-model responses suffix each variable with its model name.
function series(hourly, variable, model) {
  return hourly[variable + '_' + model] || hourly[variable] || [];
}

function firstAvailable(a, b) {
  return (a === null || a === undefined) ? b : a;
}

function roundOrUnavailable(v) {
  return (v === null || v === undefined) ? -100 : Math.round(v);
}

function sendWeather(data, model, uvHourly) {
  saveTimezone(data.timezone);
  // The current block comes from the first model requested, so it's only
  // usable when that model is the one being shown.
  var currentUsable = true;
  if (model && !modelFitsTimezone(model, data.timezone)) {
    model = null;
    currentUsable = false;
  }

  var hourly = data.hourly;
  var times = hourly.time;
  var nowSec = Date.now() / 1000;
  var idx = currentHourIndex(times, nowSec);

  var bestTemps = series(hourly, 'temperature_2m', 'best_match');
  var regionalTemps = model ? series(hourly, 'temperature_2m', model) : [];
  var precip = series(hourly, 'precipitation_probability', 'best_match');
  var fallbackUv = series(hourly, 'uv_index', 'best_match');
  var camsUv = uvHourly ? (uvHourly.uv_index || []) : [];
  var camsIdx = uvHourly ? uvHourly.time.indexOf(times[idx]) : -1;

  function tempAt(i) {
    return firstAvailable(regionalTemps[i], bestTemps[i]);
  }
  function uvAt(k) {
    return firstAvailable(camsIdx >= 0 ? camsUv[camsIdx + k] : null, fallbackUv[idx + k]);
  }

  var current = data.current || {};
  // CAMS is hourly; interpolate so NOW doesn't lag a rising or falling UV.
  var frac = Math.min(Math.max((nowSec - times[idx]) / 3600, 0), 1);
  var uv0 = uvAt(0);
  var uv1 = uvAt(1);
  var uvNow = (uv0 == null || uv1 == null) ? uv0 : uv0 + (uv1 - uv0) * frac;

  var msg = {
    WEATHER_BASE_TIME: times[idx],
    CURRENT_TIME: Math.floor(nowSec),
    CURRENT_TEMP: roundOrUnavailable(currentUsable ? current.temperature_2m : null),
    CURRENT_UV: roundOrUnavailable(uvNow)
  };
  for (var k = 0; k < FORECAST_HOURS; k++) {
    msg['HOURLY_TEMP_' + k] = roundOrUnavailable(tempAt(idx + k));
    // precipitation_probability covers the preceding hour, so the chance
    // for the hour starting at idx + k is reported at idx + k + 1.
    msg['HOURLY_PRECIP_' + k] = roundOrUnavailable(precip[idx + k + 1]);
    msg['HOURLY_UV_' + k] = roundOrUnavailable(uvAt(k));
  }

  console.log('Weather: temperature from ' + (model || 'best_match') +
    ', UV from ' + (camsIdx >= 0 ? 'CAMS' : 'forecast API'));
  Pebble.sendAppMessage(msg,
    function (e) { console.log('Weather sent successfully'); },
    function (e) { console.log('Error sending weather: ' + JSON.stringify(e)); }
  );
}

function locationSuccess(pos) {
  var lat = pos.coords.latitude;
  var lon = pos.coords.longitude;

  var forecast = null;
  var model = null;
  var uvHourly = null;
  var pending = 2;
  function done() {
    if (--pending > 0) return;
    if (!forecast) {
      console.log('Weather request failed');
      return;
    }
    sendWeather(forecast, model, uvHourly);
  }

  fetchForecast(lat, lon, regionalCandidates(lat, lon), function (data, m) {
    forecast = data;
    model = m;
    done();
  });
  fetchUv(lat, lon, function (hourly) {
    uvHourly = hourly;
    done();
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
