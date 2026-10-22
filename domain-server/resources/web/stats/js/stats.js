// A service's details: what it is and how it's doing, the numbers that matter most, who it's connected to, and every
// number it reports, in words and units rather than raw keys. Numbers update in place each second; clicking one
// opens a graph of it.
$(document).ready(function(){

  var uuid = qs("uuid");
  var currentHighchart = null;
  var graphPath = null;
  var lastStats = null;
  var lastSignature = null;
  var nodesByUUID = {};
  var thisNode = null;

  // ---------- words and units ----------

  // keys whose meaning isn't plain from their names
  var KEY_NAMES = {
    "$username": "ID",
    "inbound_kbps": "Data in",
    "outbound_kbps": "Data out",
    "inbound_pps": "Packets in",
    "outbound_pps": "Packets out",
    "inbound_kbit/s": "Data in",
    "outbound_kbit/s": "Data out",
    "numQueuedCheckIns": "Check-ins waiting",
    "nodelist_event_queue_size": "Network events waiting",
    "threads": "Threads",
    "throttling_ratio": "Throttling",
    "trailing_mix_ratio": "Time spent mixing",
    "assignmentStats": "Assignments",
    "io_stats": "Network",
    "avg_timing_stats": "Timing",
    "mix_stats": "Mixing",
    "z_listeners": "Listeners",
    "z_avatars": "Avatars",
    "nodes": "Connections",
    "messages": "Connections",
    "octree_stats": "Objects",
    "octree": "Objects",
    "script_engine_stats": "Scripts",
    "number_running_scripts": "Scripts running",
    "elementCount": "Objects",
    "internalElementCount": "Groups of objects",
    "leafElementCount": "Single objects",
    "EntityServer": "World objects",
    "misc": "General",
    "broadcast_loop_rate": "Updates sent per second",
    "average_listeners_last_second": "Listening, on average",
    "useDynamicJitterBuffers": "Smooth out uneven sound",
    "display_name": "Name",
    "node_type": "Type",
    "persistFileLoadTime": "Time to load the world",
    "detailed_stats_url": "Detailed stats",
    "clients": "Connected",
    "uptime": "Running for"
  };

  // short words in the stats' names, spelled out (and a couple of their typos)
  var WORDS = {
    "avg": "average", "av": "avatar", "avs": "avatars", "num": "number of", "recvd": "received", "procd": "processed",
    "est.": "estimated", "rtt": "round trip", "cw": "congestion window", "ack": "acknowledgements", "p": "packets",
    "pacekt": "packet", "innner": "inner", "kbit/s": "", "packet/s": "", "hrtf": "3D sound", "ms": "", "us": ""
  };

  // a key in words: "1. elementCount" -> "Element count", "us_per_frame_trailing" -> "Per frame (recent)"
  function keyName(key) {
    if (KEY_NAMES.hasOwnProperty(key)) {
      return KEY_NAMES[key];
    }
    var bare = String(key).replace(/^\d+[._]?\s*/, "");
    if (KEY_NAMES.hasOwnProperty(bare)) {
      return KEY_NAMES[bare];
    }
    var words = bare
      .replace(/^us_per_/, "per_")
      .replace(/_(kbps|pps)$/, "")
      .replace(/_trailing$/, " (recent)")
      .replace(/_(\d+)s$/, " (last $1 s)")
      .replace(/%/g, " %")
      .replace(/([a-z])([A-Z])/g, "$1 $2")
      .replace(/_+/g, " ")
      .replace(/\s*\((p\/s|ms|us|mb\/s|p)\)/gi, "")
      .replace(/\s+/g, " ")
      .trim()
      .toLowerCase()
      .split(" ")
      .map(function(word) { return WORDS.hasOwnProperty(word) ? WORDS[word] : word; })
      .filter(function(word) { return word !== ""; })
      .join(" ");
    return words.charAt(0).toUpperCase() + words.slice(1);
  }

  // the unit a key's value is in, from its name
  function keyUnit(key) {
    key = String(key);
    if (/kbps$|kbit\/s$/.test(key)) return "kbit/s";
    if (/pps$|packet\/s$/.test(key)) return "packets/s";
    if (/^us_per_|\(us\)/.test(key)) return "µs";
    if (/\(ms\)/.test(key)) return "ms";
    if (/\(Mb\/s\)/.test(key)) return "Mbit/s";
    if (/\(P\/s\)/.test(key)) return "packets/s";
    if (/\(P\)$/.test(key)) return "packets";
    if (/bytes/i.test(key)) return "bytes";
    if (key === "throttling_ratio" || key === "trailing_mix_ratio") return "ratio%";
    return "";
  }

  function formatNumber(n) {
    if (!isFinite(n)) return String(n);
    if (n === 0) return "0";
    var abs = Math.abs(n);
    if (abs >= 1000) return Math.round(n).toLocaleString();
    if (Number.isInteger(n)) return String(n);
    if (abs < 0.01) return n.toPrecision(2);
    return n.toLocaleString(undefined, { maximumFractionDigits: 2 });
  }

  // a value in words, with its unit
  function formatValue(value, key) {
    if (value === true) return "Yes";
    if (value === false) return "No";
    if (value === null || value === undefined || value === "") return "—";
    if (typeof value !== "number") return String(value);
    var unit = keyUnit(key);
    if (unit === "ratio%") return formatNumber(value * 100) + " %";
    if (unit === "kbit/s" && Math.abs(value) >= 1000) return formatNumber(value / 1000) + " Mbit/s";
    if (unit === "µs" && Math.abs(value) >= 1000) return formatNumber(value / 1000) + " ms";
    if (unit === "bytes") {
      var abs = Math.abs(value);
      if (abs >= 1e9) return formatNumber(value / 1e9) + " GB";
      if (abs >= 1e6) return formatNumber(value / 1e6) + " MB";
      if (abs >= 1e3) return formatNumber(value / 1e3) + " KB";
      return formatNumber(value) + " bytes";
    }
    return formatNumber(value) + (unit ? " " + unit : "");
  }

  function escapeHTML(text) {
    return _.escape(String(text));
  }

  // ---------- reading the stats ----------

  function valueAt(json, path) {
    var value = json;
    for (var i = 0; i < path.length; i++) {
      if (value === null || typeof value !== "object") return undefined;
      value = value[path[i]];
    }
    return value;
  }

  function isObject(value) {
    return value !== null && typeof value === "object" && !Array.isArray(value);
  }

  // an object whose entries are each a connection (they carry a $username: the connection's ID)
  function connectionEntries(object) {
    return _.filter(_.keys(object), function(key) {
      return isObject(object[key]) && object[key].hasOwnProperty("$username");
    });
  }

  // a connection's name: the service or visitor it is, from the overview's list of who's connected
  function connectionName(id, entry) {
    var node = nodesByUUID[id];
    if (entry && entry.display_name) {
      var service = Services.find(entry.display_name);
      return service ? service.name : entry.display_name;
    }
    if (entry && entry.node_type) {
      var byType = Services.find(entry.node_type);
      if (byType) return byType.name;
    }
    if (node) {
      if (node.type === "agent") return node.username || "Someone not signed in";
      var known = Services.find(node.type);
      if (known) return known.name;
    }
    return "Connection " + String(id).slice(0, 8);
  }

  // the groups of connections in the stats: the root's own (the file storage lists them there) and its objects'
  function connectionGroups(json) {
    var groups = [];
    var rootEntries = connectionEntries(json);
    if (rootEntries.length) {
      groups.push({ path: [], title: "Connections", keys: rootEntries });
    }
    _.each(_.keys(json), function(key) {
      if (isObject(json[key])) {
        var entries = connectionEntries(json[key]);
        if (entries.length && entries.length === _.keys(json[key]).length) {
          groups.push({ path: [key], title: keyName(key), keys: entries });
        }
      }
    });
    return groups;
  }

  function count(object) {
    return isObject(object) ? _.keys(object).length : 0;
  }

  // the numbers that matter most for each kind of service
  function highlightsFor(type, json) {
    var cards = [
      { label: "Data in", path: ["io_stats", "inbound_kbps"], hint: "From visitors and the other services" },
      { label: "Data out", path: ["io_stats", "outbound_kbps"], hint: "To visitors and the other services" }
    ];
    var more = {
      "audio-mixer": [
        { label: "Listening", value: count(json.z_listeners), hint: "Visitors and scripts it's mixing sound for" },
        { label: "Time spent mixing", path: ["trailing_mix_ratio"], hint: "Of each moment; near 100 % it's struggling" },
        { label: "Throttling", path: ["throttling_ratio"], hint: "Above 0 % it's cutting corners to keep up" }
      ],
      "avatar-mixer": [
        { label: "Avatars", value: count(json.z_avatars), hint: "Visitors and scripts whose avatars it shares" },
        { label: "Updates per second", path: ["broadcast_loop_rate"], hint: "How often it sends everyone's movements" },
        { label: "Throttling", path: ["throttling_ratio"], hint: "Above 0 % it's cutting corners to keep up" }
      ],
      "entity-server": [
        { label: "Objects in the world", path: ["EntityServer", "2. octree", "1. elementCount"], hint: "Everything that's been built" },
        { label: "Connected", path: ["EntityServer", "1. misc", "5. clients"], hint: "Visitors and services seeing the objects" }
      ],
      "entity-script-server": [
        { label: "Scripts running", path: ["script_engine_stats", "number_running_scripts"], hint: "Scripts attached to objects" },
        { label: "Objects", path: ["octree_stats", "elementCount"], hint: "Objects it's watching for scripts" },
        { label: "Connections", value: count(json.nodes), hint: "Services it talks to" }
      ],
      "asset-server": [
        { label: "Connected", value: connectionEntries(json).length, hint: "Visitors and services using the files" }
      ],
      "messages-mixer": [
        { label: "Connected", value: count(json.messages), hint: "Visitors and scripts passing messages" }
      ]
    };
    return cards.concat(more[type] || []);
  }

  // ---------- drawing ----------

  // a number that can be graphed, by where it is in the stats
  function valueHTML(value, path) {
    var key = path[path.length - 1];
    var classes = "dr-stat-value";
    var attrs = "";
    if (typeof value === "number") {
      classes += " graphable-stat";
      attrs = " data-path='" + escapeHTML(JSON.stringify(path)) + "' title='Show on a graph'";
    } else {
      attrs = " data-path='" + escapeHTML(JSON.stringify(path)) + "'";
    }
    return "<span class='" + classes + "'" + attrs + ">" + escapeHTML(formatValue(value, key)) + "</span>";
  }

  // several numbered entries that are all numbers (a queue per thread, say): shown as one line, with the list folded
  function isNumberSeries(object) {
    var keys = _.keys(object);
    if (keys.length < 6) return false;
    var stem = keys[0].replace(/_?\d+$/, "");
    return _.every(keys, function(key) {
      return typeof object[key] === "number" && key.replace(/_?\d+$/, "") === stem;
    });
  }

  function seriesSummary(object) {
    var values = _.values(object);
    var total = _.reduce(values, function(a, b) { return a + b; }, 0);
    if (total === 0) return values.length + ", all empty";
    return values.length + ": " + formatNumber(total) + " in all, at most " + formatNumber(_.max(values));
  }

  // rows of an object's values, with its objects as headed groups under them
  function rowsHTML(object, path, depth) {
    var keys = _.keys(object).sort(function(a, b) {
      // numbered keys ("1. misc") keep their order, the rest go alphabetically, values before groups
      var ao = isObject(object[a]) ? 1 : 0;
      var bo = isObject(object[b]) ? 1 : 0;
      if (ao !== bo) return ao - bo;
      return a.localeCompare(b, undefined, { numeric: true });
    });
    var html = "<dl class='dr-stat-rows'>";
    _.each(keys, function(key) {
      var value = object[key];
      var childPath = path.concat([key]);
      if (key === "$username") return;
      if (isObject(value)) {
        return;
      }
      if (Array.isArray(value)) {
        value = value.length ? value.join(", ") : "";
      }
      html += "<div class='dr-stat-row'><dt>" + escapeHTML(keyName(key)) + "</dt><dd>" + valueHTML(value, childPath) + "</dd></div>";
    });
    html += "</dl>";
    _.each(keys, function(key) {
      var value = object[key];
      if (!isObject(value)) return;
      var childPath = path.concat([key]);
      if (isNumberSeries(value)) {
        html += "<details class='dr-stat-group' data-id='" + escapeHTML(JSON.stringify(childPath)) + "'><summary>" +
          escapeHTML(keyName(key)) + " <span class='dr-stat-count' data-series='" + escapeHTML(JSON.stringify(childPath)) +
          "'>" + escapeHTML(seriesSummary(value)) + "</span></summary>" + rowsHTML(value, childPath, depth + 1) + "</details>";
      } else {
        html += "<div class='dr-stat-subgroup'><h4>" + escapeHTML(keyName(key)) + "</h4>" + rowsHTML(value, childPath, depth + 1) + "</div>";
      }
    });
    return html;
  }

  function sectionHTML(title, body, id, open, extra) {
    return "<details class='dr-stat-section'" + (open ? " open" : "") + " data-id='" + escapeHTML(id) + "'>" +
      "<summary><span class='dr-stat-section-title'>" + escapeHTML(title) + "</span>" + (extra || "") + "</summary>" +
      "<div class='dr-stat-section-body'>" + body + "</div></details>";
  }

  // which sections were open, so a redraw keeps them so
  function openSections() {
    var open = {};
    $('#stats-container details, #stats-connections details').each(function() {
      open[$(this).data('id')] = this.open;
    });
    return open;
  }

  function draw(json) {
    var type = json.node_type;
    var open = openSections();
    var firstDraw = _.isEmpty(open);
    var stats = _.omit(json, "node_type");
    var groups = connectionGroups(stats);
    var groupPaths = _.map(groups, function(group) { return JSON.stringify(group.path); });

    // the numbers that matter most
    var cards = "";
    _.each(highlightsFor(type, stats), function(card) {
      var value = card.path ? valueAt(stats, card.path) : card.value;
      if (value === undefined) return;
      cards += "<div class='dr-status-card'><div class='dr-status-label'>" + escapeHTML(card.label) + "</div>" +
        "<div class='dr-status-value'>" + (card.path ? valueHTML(value, card.path) : escapeHTML(formatValue(value, ""))) + "</div>" +
        "<div class='dr-status-hint'>" + escapeHTML(card.hint) + "</div></div>";
    });
    $('#stats-highlights').html(cards);

    // who it's connected to
    var connections = "";
    _.each(groups, function(group) {
      var container = group.path.length ? valueAt(stats, group.path) : stats;
      var list = "<ul class='dr-service-list'>";
      _.each(group.keys, function(id) {
        var entry = container[id];
        var path = group.path.concat([id]);
        var name = connectionName(id, entry);
        var inbound = entry.inbound_kbps !== undefined ? ["inbound_kbps"] : (entry["inbound_kbit/s"] !== undefined ? ["inbound_kbit/s"] : null);
        var outbound = entry.outbound_kbps !== undefined ? ["outbound_kbps"] : (entry["outbound_kbit/s"] !== undefined ? ["outbound_kbit/s"] : null);
        var summary = "";
        if (inbound) summary += "<span>In " + valueHTML(entry[inbound[0]], path.concat(inbound)) + "</span>";
        if (outbound) summary += "<span>Out " + valueHTML(entry[outbound[0]], path.concat(outbound)) + "</span>";
        var connectionStats = entry["Connection Stats"];
        if (isObject(connectionStats) && connectionStats["3. RTT (ms)"] !== undefined) {
          summary += "<span>Round trip " + valueHTML(connectionStats["3. RTT (ms)"], path.concat(["Connection Stats", "3. RTT (ms)"])) + "</span>";
        }
        var id_ = JSON.stringify(path);
        list += "<li class='dr-connection'><details data-id='" + escapeHTML(id_) + "'" + (open[id_] ? " open" : "") + "><summary>" +
          "<span class='dr-dot dr-dot-on'></span><span class='dr-connection-name'>" + escapeHTML(name) + "</span>" +
          "<span class='dr-connection-numbers'>" + summary + "</span></summary>" +
          "<div class='dr-connection-body'>" + rowsHTML(entry, path, 1) + "</div></details></li>";
      });
      list += "</ul>";
      connections += "<div class='panel panel-default'><div class='panel-heading'><h3 class='panel-title'>" + escapeHTML(group.title) +
        " <span class='dr-tag'>" + group.keys.length + "</span></h3><p class='panel-description'>Click one to see all its numbers.</p></div>" +
        "<div class='panel-body'>" + list + "</div></div>";
    });
    $('#stats-connections').html(connections);

    // everything else: the plain values together, then a section for each group of them
    var general = _.pick(stats, function(value, key) { return !isObject(value) && key !== "$username"; });
    var sections = "";
    if (!_.isEmpty(general)) {
      sections += sectionHTML("General", rowsHTML(general, [], 0), "[\"general\"]", firstDraw || open["[\"general\"]"]);
    }
    _.each(_.keys(stats).sort(), function(key) {
      var value = stats[key];
      if (!isObject(value) || value.hasOwnProperty("$username") || groupPaths.indexOf(JSON.stringify([key])) !== -1) return;
      var id = JSON.stringify([key]);
      var extra = isNumberSeries(value) ? " <span class='dr-stat-count' data-series='" + escapeHTML(id) + "'>" + escapeHTML(seriesSummary(value)) + "</span>" : "";
      sections += sectionHTML(keyName(key), rowsHTML(value, [key], 0), id, open[id], extra);
    });
    $('#stats-container').html(sections);

    // put the open groups back
    $('#stats-container details.dr-stat-group').each(function() {
      if (open[$(this).data('id')]) this.open = true;
    });
  }

  // the same shape of stats as last time: only the values change, in place
  function update(json) {
    var stats = _.omit(json, "node_type");
    $('[data-path]').each(function() {
      var path = JSON.parse($(this).attr('data-path'));
      var value = valueAt(stats, path);
      if (Array.isArray(value)) value = value.join(", ");
      $(this).text(formatValue(value, path[path.length - 1]));
    });
    $('[data-series]').each(function() {
      var value = valueAt(stats, JSON.parse($(this).attr('data-series')));
      if (isObject(value)) $(this).text(seriesSummary(value));
    });
  }

  // a description of the stats' shape: which keys there are, not their values
  function signature(json) {
    var keys = [];
    (function walk(object, prefix) {
      _.each(_.keys(object).sort(), function(key) {
        keys.push(prefix + key);
        if (isObject(object[key])) walk(object[key], prefix + key + "/");
      });
    })(json, "");
    return keys.join("|");
  }

  // ---------- the service itself ----------

  function showNode() {
    var type = thisNode ? thisNode.type : (lastStats ? lastStats.node_type : null);
    var service = Services.find(type);
    var title;
    var about;
    if (type === "agent") {
      title = thisNode && thisNode.username ? thisNode.username : "A visitor or script";
      about = thisNode && thisNode.username ? "Signed in to Darkride." : "Someone not signed in, or a script running on its own.";
    } else {
      title = service ? service.name : (type || "Details");
      about = service ? service.about : "";
    }
    $('#stats-title').text(title);
    $('#stats-about').text(about);
    document.title = title + " - Darkride world server";

    $('#stats-dot').toggleClass('dr-dot-on', !!thisNode).toggleClass('dr-dot-waiting', !thisNode);
    $('#stats-restart').toggle(!!thisNode && type !== "agent");

    if (thisNode) {
      var facts = [
        ["Status", "Running for " + Services.durationText(parseFloat(thisNode.uptime))],
        ["Version", thisNode.version || "—"],
        ["Address", thisNode.public ? thisNode.public.ip + ":" + thisNode.public.port : "—"],
        ["ID", uuid]
      ];
      if (thisNode.pool) facts.splice(3, 0, ["Runner group", thisNode.pool]);
      $('#stats-facts').html(_.map(facts, function(fact) {
        return "<div><dt>" + escapeHTML(fact[0]) + "</dt><dd>" + escapeHTML(fact[1]) + "</dd></div>";
      }).join(""));
    }
  }

  function getNodes() {
    $.getJSON("/nodes.json", function(json) {
      nodesByUUID = {};
      _.each(json.nodes, function(node) { nodesByUUID[node.uuid] = node; });
      thisNode = nodesByUUID[uuid] || null;
      showNode();
    });
  }

  $('#stats-restart').on('click', function() {
    var name = $('#stats-title').text();
    swal({
      title: "Restart " + name + "?",
      text: "It stops and starts again by itself, and visitors may notice a short pause. You'll go back to the overview.",
      type: "warning",
      showCancelButton: true,
      confirmButtonText: "Restart",
      closeOnConfirm: true
    }, function() {
      $.ajax({ url: "/nodes/" + uuid, type: 'DELETE' }).always(function() {
        location.href = "/";
      });
    });
  });

  // ---------- refreshing ----------

  function getNodeStats() {
    $.getJSON("/nodes/" + uuid + ".json", function(json) {
      $('#stats-stale').hide();
      lastStats = json;
      var shape = signature(json);
      if (shape !== lastSignature) {
        lastSignature = shape;
        draw(json);
        showNode();
      } else {
        update(json);
      }

      if (currentHighchart && graphPath) {
        // get the current time to set with the point
        var x = (new Date()).getTime();
        var y = Number(valueAt(_.omit(json, "node_type"), graphPath));

        // start shifting the chart once we hit 20 data points
        var shift = currentHighchart.series[0].data.length > 20;
        currentHighchart.series[0].addPoint([x, y], true, shift, false);
      }
    }).fail(function() {
      $('#stats-stale').show();
    });
  }

  getNodes();
  getNodeStats();
  setInterval(getNodeStats, 1000);
  setInterval(getNodes, 5000);

  // ---------- graphs ----------

  // the graphs' colours, from the look in use (set again when it changes, for the next graph)
  function setGraphColours() {
    var border = DarkrideTheme.colour("--dr-border");
    var muted = DarkrideTheme.colour("--dr-text-muted");
    Highcharts.setOptions({
      global: {
        useUTC: false
      },
      colors: [DarkrideTheme.colour("--dr-cyan"), DarkrideTheme.colour("--dr-violet"), DarkrideTheme.colour("--dr-green")],
      chart: {
        backgroundColor: "transparent",
        style: { fontFamily: "Roboto, 'Segoe UI', Helvetica, Arial, sans-serif" }
      },
      xAxis: {
        lineColor: border,
        tickColor: border,
        labels: { style: { color: muted } }
      },
      yAxis: {
        gridLineColor: border,
        labels: { style: { color: muted } },
        title: { style: { color: muted } }
      }
    });
  }
  setGraphColours();
  $(document).on("dr-themechange", setGraphColours);

  // the words for where a number is: "Network: Data out"
  function pathTitle(path) {
    return _.map(path, function(key) {
      var node = nodesByUUID[key];
      return node || /^[0-9a-f-]{36}$/.test(key) ? connectionName(key, valueAt(lastStats, path.slice(0, path.indexOf(key) + 1))) : keyName(key);
    }).join(": ");
  }

  function createGraphModal(path) {
    var key = path[path.length - 1];
    var unit = keyUnit(key);
    var chartModal = bootbox.dialog({
      title: pathTitle(path),
      message: "<div id='highchart-container'></div><p class='help-block'>Updates every second, while this is open.</p>",
      buttons: {},
      onEscape: true,
      backdrop: true,
      className: 'highchart-modal'
    });

    chartModal.on('hidden.bs.modal', function(e) {
      if (currentHighchart) currentHighchart.destroy();
      currentHighchart = null;
      graphPath = null;
    });

    chartModal.on('shown.bs.modal', function(e) {
      if (currentHighchart) currentHighchart.reflow();
    });

    currentHighchart = new Highcharts.Chart({
      chart: {
        renderTo: 'highchart-container',
        defaultSeriesType: 'line'
      },
      title: {
        text: ''
      },
      credits: {
        enabled: false
      },
      xAxis: {
        type: 'datetime',
        tickPixelInterval: 150,
        minTickInterval: 1000,
        labels: { format: '{value:%H:%M:%S}' }
      },
      yAxis: {
        title: {
          text: unit === "ratio%" ? "ratio" : (unit || "value")
        }
      },
      legend: {
        enabled: false
      },
      series: [{
        name: keyName(key),
        data: []
      }]
    });
  }

  // handle clicks on numerical values - this lets the user show a line graph in a modal
  $('body').on('click', '.graphable-stat', function(e){
    // a number in a connection's summary line opens the graph, not the connection
    e.preventDefault();
    e.stopPropagation();
    graphPath = JSON.parse($(this).attr('data-path'));
    createGraphModal(graphPath);
  });
});
