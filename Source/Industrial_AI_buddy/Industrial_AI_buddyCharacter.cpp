// Copyright Epic Games, Inc. All Rights Reserved.

#include "Industrial_AI_buddyCharacter.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "HttpManager.h"
#include "Misc/OutputDeviceDebug.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/StaticMeshActor.h"
#include "Sound/SoundWaveProcedural.h"
#include "AudioDevice.h"
#include "AudioCaptureCore.h"
#include "AudioCapture.h"
#include "Sound/SampleBufferIO.h" 
#include "Generators/AudioGenerator.h"
#include "Misc/Base64.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Sound/SoundWave.h"
#include "TimerManager.h"
#include "CoreMinimal.h"

DEFINE_LOG_CATEGORY(LogTemplateCharacter);

//////////////////////////////////////////////////////////////////////////
// AIndustrial_AI_buddyCharacter

AIndustrial_AI_buddyCharacter::AIndustrial_AI_buddyCharacter()
{
	//AudioCaptureInstance = nullptr;
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);
		
	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true; // Character moves in the direction of input...	
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f); // ...at this rotation rate

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 700.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f; // The camera follows at this distance behind the character	
	CameraBoom->bUsePawnControlRotation = true; // Rotate the arm based on the controller

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName); // Attach the camera to the end of the boom and let the boom adjust to match the controller orientation
	FollowCamera->bUsePawnControlRotation = false; // Camera does not rotate relative to arm

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character) 
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
	Sensitivity = 0.5f;

	//API endpoints
	FirstApi = TEXT("https://z4l7mvuhi4.execute-api.us-east-1.amazonaws.com/default/Unreal_engine_function1");
	KnowledgeBaseApi = TEXT("https://um73xjl6h0.execute-api.us-east-1.amazonaws.com/default/unreal_engine_function2");
	AgentApi = TEXT("https://zjgf2vqvhb.execute-api.us-east-1.amazonaws.com/default/unreal_engine_function3");
	SecondApi = KnowledgeBaseApi;

	//Language and mode
	LanguageText = TEXT("English");
	ModeText = TEXT("Knowledge base");
}

void AIndustrial_AI_buddyCharacter::BeginPlay()
{
	// Call the base class  
	Super::BeginPlay();

	//Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}

	setUpUI();
	// Start inactivity timer
	LastInputTime = GetWorld()->GetTimeSeconds();
	GetWorldTimerManager().SetTimer(InactivityTimerHandle, this, &AIndustrial_AI_buddyCharacter::CheckInactivity, 1.0f, true);
}
//////////////////////////////////////////////////////////////////////////

// Player Inputs
void AIndustrial_AI_buddyCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent)) {
		
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &AIndustrial_AI_buddyCharacter::OnAnyInput);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AIndustrial_AI_buddyCharacter::Move);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AIndustrial_AI_buddyCharacter::OnAnyInput);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AIndustrial_AI_buddyCharacter::Look);
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &AIndustrial_AI_buddyCharacter::OnAnyInput);

		// Print message on Enter key press via the PrintMessageAction
		EnhancedInputComponent->BindAction(PrintMessageAction, ETriggerEvent::Started, this, &AIndustrial_AI_buddyCharacter::StartAudioRecording);
		EnhancedInputComponent->BindAction(PrintMessageAction, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::StopAudioRecording);
		EnhancedInputComponent->BindAction(PrintMessageAction, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::PrintMessage);
		EnhancedInputComponent->BindAction(PrintMessageAction, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::OnAnyInput);

		//ResetGame
		EnhancedInputComponent->BindAction(ResetGameIA, ETriggerEvent::Triggered, this, &AIndustrial_AI_buddyCharacter::ResetGame);

		//Language
		EnhancedInputComponent->BindAction(LanguageIA, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::LanguageSelection);
		EnhancedInputComponent->BindAction(LanguageIA, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::OnAnyInput);

		//Mode
		EnhancedInputComponent->BindAction(ModeIA, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::ModeSelection);
		EnhancedInputComponent->BindAction(ModeIA, ETriggerEvent::Completed, this, &AIndustrial_AI_buddyCharacter::OnAnyInput);
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
	}
}

void AIndustrial_AI_buddyCharacter::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = Controller->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	
		// get right vector 
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement 
		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void AIndustrial_AI_buddyCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();
	LookAxisVector = LookAxisVector * Sensitivity;

	if (Controller != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(LookAxisVector.X);
		AddControllerPitchInput(LookAxisVector.Y);
	}
}


// Auto reseting
void AIndustrial_AI_buddyCharacter::OnAnyInput()
{
	LastInputTime = GetWorld()->GetTimeSeconds();
}

void AIndustrial_AI_buddyCharacter::CheckInactivity()
{
	float CurrentTime = GetWorld()->GetTimeSeconds();
	if (CurrentTime - LastInputTime >= InactivityDuration)
	{
		ResetGame();
		LastInputTime = CurrentTime; // Prevent repeated resets
	}
}

// Audio functions
void AIndustrial_AI_buddyCharacter::StartAudioRecording()
{
	// Play start beep sound
	if (StartBeepSound)
	{
		UGameplayStatics::PlaySound2D(GetWorld(), StartBeepSound);
	}
	
	// Reseting text Box
	TextBlock = FString::Printf(TEXT(" "));
	StatusText = FString::Printf(TEXT("|Recording Audio|"));
	changeText();


	// Audio
	bIsRecording = true;
	AudioCaptureInstance = NewObject<UAudioCapture>();
	if (AudioCaptureInstance)
	{
		// Correctly add the audio generation delegate
		AudioCaptureInstance->AddGeneratorDelegate([this](const float* InAudio, int32 NumSamples)
			{
				//FScopeLock Lock(&AudioBufferLock);
				if (bIsRecording) // Only append if still recording.
				{
					AudioBuffer.Append(InAudio, NumSamples);
				}
			});

		// Start capturing the audio
		AudioCaptureInstance->OpenDefaultAudioStream();
		AudioCaptureInstance->StartCapturingAudio();

		UE_LOG(LogTemplateCharacter, Warning, TEXT("Starting recording"));
	}
}

void AIndustrial_AI_buddyCharacter::StopAudioRecording()
{
	// Play stop beep sound
	if (StopBeepSound)
	{
		UGameplayStatics::PlaySound2D(GetWorld(), StopBeepSound);
	}
	
	if (AudioCaptureInstance)
	{
		bIsRecording = false;
		// Add critical section
		FScopeLock Lock(&AudioBufferLock);

		AudioCaptureInstance->StopCapturingAudio();

		//Destroy the audio capture object
		AudioCaptureInstance->ConditionalBeginDestroy();
		AudioCaptureInstance = nullptr;

		//delay to let OS release resources
		FPlatformProcess::Sleep(0.05f); // 50ms

		UE_LOG(LogTemplateCharacter, Warning, TEXT("AudioCapture stopped."));
		StatusText = FString::Printf(TEXT(" "));
		changeText();
	}
}

void AIndustrial_AI_buddyCharacter::PlayBase64Audio(const FString& Base64AudioData)
{
	// Decode the Base64 audio data to a byte array
	TArray<uint8> DecodedAudioData;
	FBase64::Decode(Base64AudioData, DecodedAudioData);

	// Create a USoundWaveProcedural object and populate it with the decoded audio data
	USoundWaveProcedural* SoundWave = NewObject<USoundWaveProcedural>();
	SoundWave->SetSampleRate(16000); // Set the sample rate, adjust as needed
	SoundWave->NumChannels = 1; // Set the number of channels, adjust as needed
	SoundWave->Duration = (float)DecodedAudioData.Num() / (SoundWave->GetSampleRateForCurrentPlatform() * SoundWave->NumChannels);
	SoundWave->SoundGroup = SOUNDGROUP_Default;
	SoundWave->bLooping = false;

	// Ensure the buffer size is a multiple of the sample byte size
	int32 SampleByteSize = sizeof(int16); // Assuming 16-bit audio
	int32 BufferSize = DecodedAudioData.Num();
	if (BufferSize % SampleByteSize != 0)
	{
		BufferSize -= BufferSize % SampleByteSize;
	}

	// Fill the sound wave with the decoded audio data
	SoundWave->QueueAudio(DecodedAudioData.GetData(), BufferSize);

	// Play the USoundWave object
	UGameplayStatics::PlaySound2D(GetWorld(), SoundWave);
}

// Helper function to get the closest static mesh name near the character
FString AIndustrial_AI_buddyCharacter::GetClosestMeshName()
{
	FString ClosestMeshName = "None";
	FString MeshName = "None";
	float ClosestDistanceSq = FLT_MAX;
	FVector MyLocation = GetActorLocation();

	TArray<AActor*> MeshActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), AStaticMeshActor::StaticClass(), MeshActors);

	for (AActor* Actor : MeshActors)
	{
		float DistSq = FVector::DistSquared(Actor->GetActorLocation(), MyLocation);
		if (AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor))
		{
			if (MeshActor->GetStaticMeshComponent() && MeshActor->GetStaticMeshComponent()->GetStaticMesh())
			{
				MeshName = MeshActor->GetStaticMeshComponent()->GetStaticMesh()->GetName();
			}
		}

		if (DistSq < ClosestDistanceSq && (MeshName.Left(4) == "PRIN" || MeshName.Left(4) == "SPOR"))
		{
			ClosestDistanceSq = DistSq;
			ClosestMeshName = MeshName;
		}
	}
	

	if (ClosestDistanceSq < 500000.f)
	{
		UE_LOG(LogTemplateCharacter, Warning, TEXT("Closest Mesh: %s, Distance: %f"), *ClosestMeshName, ClosestDistanceSq);
		return ClosestMeshName;
	}
	else {
		UE_LOG(LogTemplateCharacter, Warning, TEXT("No near object detected"));
		return "none";
	}
	
}

FString AIndustrial_AI_buddyCharacter::GetPlayerPosition()
{
	FVector PlayerLocation = GetActorLocation();
	return PlayerLocation.ToString();
}

// Api calls
FString AIndustrial_AI_buddyCharacter::OnResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	if (bWasSuccessful && Response.IsValid())
	{
		return Response->GetContentAsString();
	}
	else
	{
		UE_LOG(LogTemplateCharacter, Error, TEXT("Request failed"));
		return FString();
	}
}

void AIndustrial_AI_buddyCharacter::MakeApiCall(const FString& ApiUrl, const FString& Content, TFunction<void(const FString&)> OnResponse)
{
	// Create the HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->OnProcessRequestComplete().BindLambda([this, OnResponse](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
		{
			FString ApiResponse = OnResponseReceived(Request, Response, bWasSuccessful);
			OnResponse(ApiResponse);
		});
	Request->SetURL(ApiUrl);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Content);

	// Send the request
	Request->ProcessRequest();
}

// Modify Send message function
void AIndustrial_AI_buddyCharacter::PrintMessage(const FInputActionValue& Value)
{

	// Logging Audio
	UE_LOG(LogTemplateCharacter, Warning, TEXT("Sending Audio. Current lenght: %d"), AudioBuffer.Num());

	// Convert AudioBuffer to Base64
	TArray<uint8> AudioData;
	AudioData.SetNumUninitialized(AudioBuffer.Num() * sizeof(float));
	FMemory::Memcpy(AudioData.GetData(), AudioBuffer.GetData(), AudioBuffer.Num() * sizeof(float));
	FString EncodedAudioData = FBase64::Encode(AudioData);
	
	FString MeshName = GetClosestMeshName();
	FString Location = GetPlayerPosition();
	FVector CurrentPosition = GetActorLocation();

	// Check if the player has moved more than 1500 units
	if (FVector::DistSquared(CurrentPosition, LastKnownPosition) > FMath::Square(400.0f))
	{
		// Update the last known position and reset the SessionId
		LastKnownPosition = CurrentPosition;
		SessionId.Empty();
	}

	if (MeshName != "No near object detected") {
		//UE_LOG(LogTemplateCharacter, Warning, TEXT("Calling lambda"));

		// Set the request content for the first API call
		FString RequestContent = FString::Printf(TEXT("{\"audioData\":\"%s\", \"language\":\"%s\"}"), *EncodedAudioData, *LanguageText);

		// Make the first API call
		MakeApiCall(FirstApi, RequestContent, [this, MeshName, Location](const FString& ApiResponse1)
			{
				//UE_LOG(LogTemplateCharacter, Warning, TEXT("Lambda 1: %s"), *ApiResponse1);

				// Parse the JSON response
				TSharedPtr<FJsonObject> JsonObject;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ApiResponse1);

				if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
				{
					FString Transcript = JsonObject->GetStringField("transcript");
					TextBlock = FString::Printf(TEXT("User: %s"), *Transcript);
					StatusText = FString::Printf(TEXT("|Assistant thinking|"));
					changeText();

					// Display the transcript on the screen
					if (GEngine && false)
					{
						GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Yellow, FString::Printf(TEXT("User: %s"), *Transcript));
					}

					// Get the closest mesh name
					

					// Set the request content for the second API call using the response from the first API call
					FString RequestContent2;
					if (SessionId.IsEmpty())
					{
						UE_LOG(LogTemplateCharacter, Warning, TEXT("New session ID"));
						RequestContent2 = FString::Printf(TEXT("{\"prompt\":\"%s\", \"location\":\"%s\", \"meshName\":\"%s\", \"language\":\"%s\"}"), *Transcript, *Location, *MeshName, *LanguageText);
					}
					else
					{
						RequestContent2 = FString::Printf(TEXT("{\"prompt\":\"%s\", \"location\":\"%s\", \"meshName\":\"%s\", \"sessionid\":\"%s\", \"language\":\"%s\"}"), *Transcript, *Location, *MeshName, *SessionId, *LanguageText);
					}

					// Make the second API call
					MakeApiCall(SecondApi, RequestContent2, [this](const FString& ApiResponse2)
							{
								// Display the second response on the screen
								TSharedPtr<FJsonObject> JsonObject2;
								TSharedRef<TJsonReader<>> Reader2 = TJsonReaderFactory<>::Create(ApiResponse2);

								if (FJsonSerializer::Deserialize(Reader2, JsonObject2) && JsonObject2.IsValid())
								{
									SessionId = JsonObject2->GetStringField("session_id");
									FString GeneratedResponse = JsonObject2->GetStringField("generated_response");
									FString AudioData = JsonObject2->GetStringField("audio_data");
									FString Responsetime = JsonObject2->GetStringField("executionTime");

									// Display the session_id and generated_response on the screen
									if (GEngine && false)
									{
										GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Cyan, FString::Printf(TEXT("AIBuddy: %s"), *GeneratedResponse));
									}
									TextBlock = FString::Printf(TEXT("AI Buddy: %s"), *GeneratedResponse);
									StatusText = FString::Printf(TEXT(""));
									changeText();
									PlayBase64Audio(AudioData);									
									UE_LOG(LogTemplateCharacter, Warning, TEXT("Time: %s."), *Responsetime);
								}
								else
								{
									UE_LOG(LogTemplateCharacter, Error, TEXT("Failed to parse JSON response from ApiResponse2"));
								}
							});
					}
				else
				{
					UE_LOG(LogTemplateCharacter, Error, TEXT("Failed to parse JSON response"));
				}
			});
	}
	AudioBuffer.Reset();
	UE_LOG(LogTemplateCharacter, Warning, TEXT("Audio cleared. Current lenght: %d"), AudioBuffer.Num());
}

//Language selection function
void AIndustrial_AI_buddyCharacter::LanguageSelection(const FInputActionValue& Value)
{
	static int32 LanguageIndex = 0;
	TArray<FString> Languages = { TEXT("English"), TEXT("Japanese"), TEXT("Spanish") };

	// Update the LanguageText based on the current index
	LanguageText = Languages[LanguageIndex];

	// Increment the index and wrap around if necessary
	LanguageIndex = (LanguageIndex + 1) % Languages.Num();

	// Call changeText() to update the displayed text
	changeText();
}

void AIndustrial_AI_buddyCharacter::ModeSelection(const FInputActionValue& Value)
{
	static int32 ModeIndex = 0;
	TArray<FString> Modes = { TEXT("Knowledge base"), TEXT("Agent")};

	// Update the LanguageText based on the current index
	ModeText = Modes[ModeIndex];

	// Increment the index and wrap around if necessary
	ModeIndex = (ModeIndex + 1) % Modes.Num();

	if (ModeText == "Knowledge base") {
		SecondApi = KnowledgeBaseApi;
	}
	else if (ModeText == "Agent")
	{
		SecondApi = AgentApi;
	}

	// Call changeText() to update the displayed text
	SessionId.Empty();
	changeText();
}
