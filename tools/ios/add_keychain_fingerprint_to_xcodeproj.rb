#!/usr/bin/env ruby
# One-shot: adds KeychainFingerprint.swift to the Runner target's Sources.
# Re-running is safe — checks for an existing entry first.
require 'xcodeproj'

project_path = File.expand_path('../../ios/Runner.xcodeproj', __dir__)
project = Xcodeproj::Project.open(project_path)

runner_target = project.targets.find { |t| t.name == 'Runner' }
raise 'Runner target not found' unless runner_target

runner_group = project.main_group.find_subpath('Runner', false)
raise 'Runner group not found' unless runner_group

swift_file_name = 'KeychainFingerprint.swift'
existing_ref = runner_group.files.find { |f| f.path == swift_file_name }
if existing_ref
  puts "#{swift_file_name} already in project — skipping."
else
  file_ref = runner_group.new_reference(swift_file_name)
  file_ref.include_in_index = '1'
  file_ref.last_known_file_type = 'sourcecode.swift'
  runner_target.source_build_phase.add_file_reference(file_ref)
  puts "Added #{swift_file_name} to Runner/Sources."
end

project.save
puts 'Saved project.pbxproj.'
