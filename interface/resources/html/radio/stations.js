//
//  stations.js
//  interface/resources/html/radio
//
//  The radio's built-in stations: the nine SomaFM channels on their Link to Us page (somafm.com/linktous), in its
//  order, names as plain text. Each one's addresses are the ones in its official playlist file (somafm.com/<id>.pls,
//  read 2026-10-31): the radio makes no calls to SomaFM's API (their rules, team/skunkworks/briefs/radio.md).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

window.RADIO_STATIONS = [
    { name: "Groove Salad", page: "https://somafm.com/groovesalad", playlist: "https://somafm.com/groovesalad.pls",
      streams: ["https://ice6.somafm.com/groovesalad-128-mp3", "https://ice2.somafm.com/groovesalad-128-mp3", "https://ice5.somafm.com/groovesalad-128-mp3"] },
    { name: "Secret Agent", page: "https://somafm.com/secretagent", playlist: "https://somafm.com/secretagent.pls",
      streams: ["https://ice6.somafm.com/secretagent-128-mp3", "https://ice2.somafm.com/secretagent-128-mp3", "https://ice5.somafm.com/secretagent-128-mp3"] },
    { name: "Drone Zone", page: "https://somafm.com/dronezone", playlist: "https://somafm.com/dronezone.pls",
      streams: ["https://ice6.somafm.com/dronezone-128-mp3", "https://ice2.somafm.com/dronezone-128-mp3", "https://ice5.somafm.com/dronezone-128-mp3"] },
    { name: "Boot Liquor", page: "https://somafm.com/bootliquor", playlist: "https://somafm.com/bootliquor.pls",
      streams: ["https://ice6.somafm.com/bootliquor-128-mp3", "https://ice2.somafm.com/bootliquor-128-mp3", "https://ice5.somafm.com/bootliquor-128-mp3"] },
    { name: "Indie Pop Rocks!", page: "https://somafm.com/indiepop", playlist: "https://somafm.com/indiepop.pls",
      streams: ["https://ice6.somafm.com/indiepop-128-mp3", "https://ice2.somafm.com/indiepop-128-mp3", "https://ice5.somafm.com/indiepop-128-mp3"] },
    { name: "Beat Blender", page: "https://somafm.com/beatblender", playlist: "https://somafm.com/beatblender.pls",
      streams: ["https://ice6.somafm.com/beatblender-128-mp3", "https://ice2.somafm.com/beatblender-128-mp3", "https://ice5.somafm.com/beatblender-128-mp3"] },
    { name: "Sonic Universe", page: "https://somafm.com/sonicuniverse", playlist: "https://somafm.com/sonicuniverse.pls",
      streams: ["https://ice6.somafm.com/sonicuniverse-128-mp3", "https://ice2.somafm.com/sonicuniverse-128-mp3", "https://ice5.somafm.com/sonicuniverse-128-mp3"] },
    { name: "The Trip", page: "https://somafm.com/thetrip", playlist: "https://somafm.com/thetrip.pls",
      streams: ["https://ice6.somafm.com/thetrip-128-mp3", "https://ice2.somafm.com/thetrip-128-mp3", "https://ice5.somafm.com/thetrip-128-mp3"] },
    { name: "cliqhop", page: "https://somafm.com/cliqhop", playlist: "https://somafm.com/cliqhop.pls",
      streams: ["https://ice6.somafm.com/cliqhop-128-mp3", "https://ice2.somafm.com/cliqhop-128-mp3", "https://ice5.somafm.com/cliqhop-128-mp3"] },
];
