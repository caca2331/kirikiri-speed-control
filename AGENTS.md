## MOST IMPORTANT
- (sandbox_mode = "workspace-write") NEVER change/remove/add files outside the workspace.
- (approval_policy = "never") Don't ask for approval for making changes within the workspace, you have the approval. 
- (features.web_search_request = true) You are granted permission of network access for searching solutions and documentations. 
- (sandbox_workspace_write.network_access = false) Don't Download anything or install any packages from the Internet. If essential, ask for permissions.

## General rules
- (first time only) Read AGENTS.md (this file), guide.md, README.md;
AGENTS.md defines the overall agent behavior.
guide.md has the goal of the project.
README.md is purposed for instructing the users.
runtime-notes.md records key observations/ideas that are not explained elsewhere regarding to the project/tasks that you discovered. Append note on it when you think necessary. You don't need to read it though unless asked.
draft.md is for my own purpose. ignore it.
- If task is ambiguous, ask for clarifications if necessary before executing tasks.
- After finish a task, update Readme if necessary. 
- Log essential information, and use them for debugging.
- Notify me in your next response when you did a context window length compress.
- Unless you have to, prevent small and frequent patches, to reduce the amount of work of mine testing.
- Make source files concise and organized. Try keep each file below 1000 lines, or by best below 500 lines.
- Put instructions in this file in high priority. Keep them in your context window.

## Rules for this project
- (First time only) Make sure you read all files in src and cmake folder.
- For this project, log files should be located in ./dist/KrkrSpeedController/ (controller root) by default
- It could be hard for you to fully detect the result. The interaction would be: I give you instructions -> you think deeply, check for information (look at log, code, online resources/documentations, etc.), then ask for clarification if any -> I answer -> You start coding, make the executable and stage to dist/ if there's code change, finally tell me what you did (not detail, but the key summary) and instruct me how to test. (during which, follow AGENTS.md) -> I run test, generate log file, give you instructions.
