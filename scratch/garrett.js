// A file to test what happens when we cannot provide enough samples
var fs = require('fs');
const { Readable, finished: streamFinished } = require('stream');
const util = require('util');
const { once } = require('events');

//var portAudio = require('naudiodon');
var portAudio = require('../index.js');

function runAudio(fileName, sleepTime) {
  //console.log(portAudio.getDevices());
  var ao = new portAudio.AudioIO({
    outOptions: {
      channelCount: 1,
      sampleFormat: portAudio.SampleFormat32Bit,
      sampleRate: 48000,
      deviceId: -1, // Use -1 or omit the deviceId to select the default device
      closeOnError: true, // Close the stream if an audio error is detected, if set false then just log the error
    },
  });
  ao.on('finish', () => {
    console.log('ERROR');
  });

  //// Create a stream to pipe into the AudioOutput
  //// Note that this does not strip the WAV header so a click will be heard at the beginning
  var rs = fs.createReadStream(fileName);
  //function readableStream(data) {
  //let index = 0;
  //return {
  //next: function() {
  //console.log('get data');
  //return {
  //value: data,
  //done: false
  //};
  //},
  //[Symbol.iterator]: function() { return this; }
  //};
  //}
  //const readable = Readable.from(idMaker());
  ////console.log(rs);

  const finished = util.promisify(streamFinished); // (A)

  // USE 2.2.5!!!
  // https://2ality.com/2019/11/nodejs-streams-async-iteration.html
  //ao.start();
  function write(data) {
    return new Promise(resolve => {
      ao.write(data, null, resolve);
    });
  }
  function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
  }
  async function pipeFileToSpeakers() {
    let i = 0;
    let timeDeltaMs = 0;
    let prevTimeMs = Date.now();
    for await (const chunk of rs) {
      const currentTime = Date.now();
      const chunkMs = chunk.length / 4 / 48;
      timeDeltaMs += currentTime - prevTimeMs - chunkMs;
      prevTimeMs = currentTime;
      //console.log('time delta:', timeDeltaMs);
      if (i === 20) {
        console.log('sleep-o');
        await sleep(sleepTime);
      }

      //console.log('before write', Date.now());
      const result = await write(chunk);
      //console.log('after write', Date.now());
      //await sleep(100);
      //console.log('result:', String(result));
      if (result === false) {
        // Handle backpressure
        console.log('waito');
        await once(ao, 'drain');
        console.log('drained--time to write more!');
      }
      i++;
    }
    ao.end(); // (C)
    // Wait until done. Throws if there are errors.
    await finished(ao);
  }

  //// Start piping data and start streaming
  rs.once('readable', async () => {
    console.log('STAHT');
    ao.start(Date.now() - 2000);
    await pipeFileToSpeakers();
  });
}
//runAudio('test.wav', 0);
runAudio('test.wav', 4000);
