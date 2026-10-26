file(MAKE_DIRECTORY "${OUTPUT}")
function(generate name)
  execute_process(COMMAND "${FFMPEG}" -hide_banner -loglevel error -y ${ARGN} "${OUTPUT}/${name}"
    RESULT_VARIABLE result ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture ${name}: ${error}")
  endif()
endfunction()
generate(av.mp4 -f lavfi -i testsrc2=size=160x90:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100
  -t 2.4 -c:v mpeg4 -bf 2 -g 25 -pix_fmt yuv420p -c:a aac -shortest)
generate(audio.wav -f lavfi -i sine=frequency=880:sample_rate=44100 -t 0.35 -c:a pcm_s16le)
generate(video.mp4 -f lavfi -i testsrc2=size=160x90:rate=25 -t 0.4 -c:v mpeg4 -bf 2 -an)
generate(offset.ts -f lavfi -i testsrc2=size=160x90:rate=25 -t 0.4 -c:v mpeg2video -bf 2 -an -output_ts_offset 7)
file(WRITE "${OUTPUT}/invalid.mp4" "not a media container")
