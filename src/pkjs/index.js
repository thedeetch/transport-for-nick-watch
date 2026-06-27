// Import Clay library
var Clay = require('pebble-clay');
var clayConfig = require('./config');
var clay = new Clay(clayConfig);

// Keep track of the current TfL API key
var tflApiKey = "";

// Function to load settings
function loadSettings() {
    var storedKey = localStorage.getItem('TFL_API_KEY');
    if (storedKey) {
        tflApiKey = storedKey;
    }
}

// Function to handle AppMessage requests from C side
Pebble.addEventListener('ready', function(e) {
    console.log('PebbleKit JS ready!');
    loadSettings();
    // Fetch arrivals on launch
    fetchLocationAndArrivals();
});

// Watch received an AppMessage
Pebble.addEventListener('appmessage', function(e) {
    console.log('Received message from watch: ' + JSON.stringify(e.payload));
    var payload = e.payload;
    if (payload.TFL_API_KEY !== undefined) {
        tflApiKey = payload.TFL_API_KEY;
        localStorage.setItem('TFL_API_KEY', tflApiKey);
        console.log('Updated TfL API Key from watch: ' + tflApiKey);
    }
    if (payload.REQ_UPDATE !== undefined) {
        console.log('Watch requested update');
        fetchLocationAndArrivals();
    }
});

// Handle webview closed (settings saved)
Pebble.addEventListener('webviewclosed', function(e) {
    console.log('Webview closed');
    loadSettings();
    // Notify C side about updated settings or fetch immediately
    fetchLocationAndArrivals();
});

// Send status message to C side
function sendStatus(message) {
    Pebble.sendAppMessage({
        "STATUS_MSG": message
    }, function() {
        console.log('Sent status to watch: ' + message);
    }, function(e) {
        console.log('Failed to send status to watch: ' + JSON.stringify(e));
    });
}

// Send clean slate message to C side before sending new batch of arrivals
function sendClean() {
    Pebble.sendAppMessage({
        "DATA_CLEAN": 1
    }, function() {
        console.log('Sent clean command to watch');
    }, function(e) {
        console.log('Failed to send clean command: ' + JSON.stringify(e));
    });
}

// Send an arrival item to the C side
function sendArrival(index, count, station, route, direction, eta, type) {
    var dict = {
        "DATA_INDEX": index,
        "DATA_COUNT": count,
        "DATA_STATION": station.substring(0, 31), // limit length for C buffer safety
        "DATA_ROUTE": route.substring(0, 15),
        "DATA_DIRECTION": direction.substring(0, 31),
        "DATA_ETA": parseInt(eta),
        "DATA_TYPE": parseInt(type) // 0 for Tube, 1 for Bus
    };
    Pebble.sendAppMessage(dict, function() {
        console.log('Sent arrival ' + index + '/' + count + ' to watch');
    }, function(err) {
        console.log('Failed to send arrival ' + index + ': ' + JSON.stringify(err));
        // Retry once
        setTimeout(function() {
            Pebble.sendAppMessage(dict, function() {}, function() {});
        }, 1000);
    });
}

// Fetch geolocation and then fetch TfL API
function fetchLocationAndArrivals() {
    sendStatus("Getting location...");
    navigator.geolocation.getCurrentPosition(
        function(position) {
            var lat = position.coords.latitude;
            var lon = position.coords.longitude;
            console.log('Got GPS: ' + lat + ', ' + lon);
            getTfLArrivals(lat, lon);
        },
        function(err) {
            console.log('GPS error: ' + JSON.stringify(err) + '. Using default coords (London Central)');
            // Fallback to central London coordinates if GPS fails
            getTfLArrivals(51.5074, -0.1278);
        },
        { enableHighAccuracy: true, timeout: 15000, maximumAge: 10000 }
    );
}

// Core TfL API logic
function getTfLArrivals(lat, lon) {
    sendStatus("Fetching stops...");
    
    // We want both Tube (NaptanMetroStation) and Bus (NaptanPublicBusCoachTram) stop types.
    // Querying within a 600-meter radius to find nearest StopPoints.
    var stopTypes = "NaptanMetroStation,NaptanPublicBusCoachTram";
    var radius = 600;
    var url = "https://api.tfl.gov.uk/StopPoint?lat=" + lat + "&lon=" + lon + 
              "&stopTypes=" + stopTypes + "&radius=" + radius + "&useStopPointHierarchy=true";
    
    if (tflApiKey && tflApiKey.trim().length > 0) {
        url += "&app_key=" + encodeURIComponent(tflApiKey.trim());
    }
    
    console.log('Requesting StopPoints: ' + url);
    
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url);
    xhr.onload = function() {
        if (xhr.status === 200) {
            try {
                var response = JSON.parse(xhr.responseText);
                var stopPoints = response.stopPoints || [];
                if (stopPoints.length === 0) {
                    sendStatus("No nearby stops found");
                    return;
                }
                
                // Group stop points by Mode (Tube / Bus) and find closest ones
                var tubeStops = [];
                var busStops = [];
                
                for (var i = 0; i < stopPoints.length; i++) {
                    var sp = stopPoints[i];
                    var isTube = false;
                    var isBus = false;
                    
                    if (sp.modes) {
                        if (sp.modes.indexOf('tube') > -1 || sp.modes.indexOf('underground') > -1 || sp.modes.indexOf('dlr') > -1 || sp.modes.indexOf('overground') > -1 || sp.modes.indexOf('elizabeth-line') > -1) {
                            isTube = true;
                        }
                        if (sp.modes.indexOf('bus') > -1) {
                            isBus = true;
                        }
                    }
                    
                    // Also check stopType if modes didn't classify it clearly
                    if (sp.stopType === 'NaptanMetroStation') {
                        isTube = true;
                    } else if (sp.stopType === 'NaptanPublicBusCoachTram' || sp.stopType === 'NaptanOnstreetBusCoachStopPair') {
                        isBus = true;
                    }
                    
                    var distance = sp.distance || 9999;
                    var stopInfo = {
                        id: sp.id,
                        name: sp.commonName,
                        distance: distance,
                        isTube: isTube,
                        isBus: isBus
                    };
                    
                    if (isTube) tubeStops.push(stopInfo);
                    if (isBus) busStops.push(stopInfo);
                }
                
                // Sort by distance (closest first)
                tubeStops.sort(function(a, b) { return a.distance - b.distance; });
                busStops.sort(function(a, b) { return a.distance - b.distance; });
                
                // Select nearest tube station and nearest bus stops (up to 2 bus stops and 1 tube station to keep it balanced)
                var selectedStops = [];
                
                if (tubeStops.length > 0) {
                    selectedStops.push(tubeStops[0]);
                    console.log('Selected Tube Stop: ' + tubeStops[0].name + ' (' + tubeStops[0].id + ')');
                }
                for (var b = 0; b < Math.min(busStops.length, 2); b++) {
                    selectedStops.push(busStops[b]);
                    console.log('Selected Bus Stop: ' + busStops[b].name + ' (' + busStops[b].id + ')');
                }
                
                if (selectedStops.length === 0) {
                    sendStatus("No tube/bus stops found");
                    return;
                }
                
                // Fetch arrivals for all selected stops
                fetchArrivalsForStops(selectedStops);
                
            } catch (ex) {
                console.log('Error parsing StopPoints JSON: ' + ex);
                sendStatus("Parse error (stops)");
            }
        } else {
            console.log('StopPoints API error: ' + xhr.status + ' - ' + xhr.statusText);
            sendStatus("StopPoint API Error");
        }
    };
    xhr.onerror = function() {
        sendStatus("Network Error");
    };
    xhr.send();
}

// Fetch arrivals for multiple selected stops, sort them, and send to C
function fetchArrivalsForStops(stops) {
    sendStatus("Fetching arrivals...");
    var allArrivals = [];
    var pendingRequests = stops.length;
    
    if (pendingRequests === 0) {
        sendStatus("No selected stops");
        return;
    }
    
    stops.forEach(function(stop) {
        var url = "https://api.tfl.gov.uk/StopPoint/" + stop.id + "/Arrivals";
        if (tflApiKey && tflApiKey.trim().length > 0) {
            url += "?app_key=" + encodeURIComponent(tflApiKey.trim());
        }
        
        console.log('Requesting arrivals for ' + stop.name + ': ' + url);
        
        var xhr = new XMLHttpRequest();
        xhr.open('GET', url);
        xhr.onload = function() {
            pendingRequests--;
            if (xhr.status === 200) {
                try {
                    var arrivals = JSON.parse(xhr.responseText) || [];
                    console.log('Got ' + arrivals.length + ' arrivals for stop ' + stop.name);
                    
                    arrivals.forEach(function(arr) {
                        // Keep track of arrival details
                        var stationName = stop.name;
                        var route = arr.lineName || "?";
                        var direction = arr.towards || arr.destinationName || "Unknown";
                        var etaSeconds = arr.timeToStation || 0; // ETA in seconds
                        var type = stop.isTube ? 0 : 1; // 0 for tube, 1 for bus
                        
                        allArrivals.push({
                            station: stationName,
                            route: route,
                            direction: direction,
                            eta: Math.round(etaSeconds / 60), // ETA in minutes
                            type: type,
                            distance: stop.distance
                        });
                    });
                } catch (e) {
                    console.log('Error parsing arrivals for ' + stop.name + ': ' + e);
                }
            } else {
                console.log('Arrivals API error for ' + stop.name + ': ' + xhr.status);
            }
            
            // Once all requests are complete, process and send
            if (pendingRequests === 0) {
                processAndSendArrivals(allArrivals);
            }
        };
        xhr.onerror = function() {
            pendingRequests--;
            console.log('Network error fetching arrivals for ' + stop.name);
            if (pendingRequests === 0) {
                processAndSendArrivals(allArrivals);
            }
        };
        xhr.send();
    });
}

function processAndSendArrivals(arrivals) {
    if (arrivals.length === 0) {
        sendStatus("No upcoming arrivals");
        return;
    }
    
    // Sort arrivals by:
    // 1. Station Name (grouping by station)
    // 2. ETA (ascending, so soonest is first)
    arrivals.sort(function(a, b) {
        if (a.station !== b.station) {
            return a.station < b.station ? -1 : 1;
        }
        return a.eta - b.eta;
    });
    
    // Limit to max 12 arrivals to prevent overloading Pebble's limited memory/AppMessage queue
    var maxArrivals = Math.min(arrivals.length, 12);
    console.log('Sending ' + maxArrivals + ' sorted arrivals to watch...');
    
    // Send clean command first
    sendClean();
    
    // Send arrivals one by one with a small delay to avoid packet loss
    var sendIndex = 0;
    
    function sendNext() {
        if (sendIndex < maxArrivals) {
            var arr = arrivals[sendIndex];
            sendArrival(sendIndex, maxArrivals, arr.station, arr.route, arr.direction, arr.eta, arr.type);
            sendIndex++;
            setTimeout(sendNext, 400); // 400ms delay between messages
        } else {
            sendStatus("Done");
        }
    }
    
    // Start sending after a brief delay to allow clean command to process
    setTimeout(sendNext, 500);
}
